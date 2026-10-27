// Pitch Shift (sfppack1's CSfPitchs) against what the DLL itself produces: the outputs below, made by running the DLL's own
// stream engine on the test signal of vegasfx_script.h (behind as many silent frames as the plug-in reports as its delay,
// each segment between two resets by itself), must come out of Reno's version bit for bit.
// The anti-alias filter (ratio above 1.1 with the box ticked) is included; it rounds tiny values to zero once per block, so
// its cases were made with the DLL fed 512-frame blocks, as Reno feeds them.
//
//   vegasfx_pitch_test
#include "vegasfx_script.h"
#include "../../src/audiofx/vegaspitch.h"

static const vegfx::Case kCases[] = {
    {"44100 up7", "rate 44100\nch 2\nparams 7 0 2 1 0 0 -680996944 1073215760 0 0 0 1094987768\nreset\nproc 100000\n", 0xbac9f860e17c94ceull, 200000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xbc1acbba, 0x3e51f77c, 0xbba9c863, 0xbba89b8b}},
    {"44100 down5", "rate 44100\nch 2\nparams -5 0 2 1 0 0 -680996944 1072167184 0 0 0 1094987768\nreset\nproc 100000\n", 0xa33e48cc8dbbb2f9ull, 200000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x3e337f2c, 0x3d89b147, 0x3f02d3ad, 0x3f02ddb8}},
    {"44100 cents lin", "rate 44100\nch 2\nparams 3 250 1 1 7 0 -1310443107 1072909783 0 0 0 1094987768\nreset\nproc 100000\n", 0xcc1acdaf1f2f6e95ull, 200000, {0x00000000, 0x00000000, 0x3e50e4ce, 0x3e510cce, 0x3dccb41c, 0x3d696a04, 0xbd922747, 0xbd921347}},
    {"44100 acc3 mode12", "rate 44100\nch 2\nparams -3 -250 3 1 12 0 -740305482 1072334300 0 0 0 1094987768\nreset\nproc 100000\n", 0xa900015d35b8f1a3ull, 200000, {0x00000000, 0x00000000, 0xbe0f597f, 0xbe0f4f92, 0xbd7a8a08, 0x3d7800ad, 0x3e5b4d2b, 0x3e5b6f97}},
    {"44100 drums up12", "rate 44100\nch 2\nparams 12 0 2 1 18 0 0 1073741824 0 0 0 1094987768\nreset\nproc 100000\n", 0xd49c148bdb2fc05dull, 200000, {0x00000000, 0x00000000, 0xbea0e5ce, 0xbea0d1ce, 0x3d44ec00, 0x3e8a0800, 0xbdcf3200, 0xbdcf1e00}},
    {"44100 speech down3", "rate 44100\nch 2\nparams -3 0 2 1 8 0 -1722100819 1072359583 0 0 0 1094987768\nreset\nproc 100000\n", 0x357bd5a13277066eull, 200000, {0x00000000, 0x00000000, 0x3d8dbd5f, 0x3d8dd168, 0xbdc35e79, 0x3deff044, 0x3d8fd907, 0x3d9028ed}},
    {"44100 low aa flag", "rate 44100\nch 2\nparams -7 0 2 1 3 0 150006878 1071995777 1 0 0 1094987768\nreset\nproc 100000\n", 0x6a0843b2d4016ce5ull, 200000, {0x00000000, 0x00000000, 0x3dcaf22c, 0x3dcb4229, 0xbea69bd5, 0xbbb53438, 0xbd98dfea, 0xbd98cbcf}},
    {"44100 aa up7", "rate 44100\nch 2\nparams 7 0 2 1 0 0 -680996944 1073215760 1 0 0 1094987768\nreset\nproc 100000\n", 0x77e2a1a0ec89e018ull, 200000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x3dc5a0f6, 0x3e8144e7, 0xbd45adc3, 0xbd458704}},
    {"44100 aa up12 acc3", "rate 44100\nch 2\nparams 12 0 3 1 12 0 0 1073741824 1 0 0 1094987768\nreset\nproc 100000\n", 0x05947f89e6ff5259ull, 200000, {0x00000000, 0x00000000, 0xbda8da52, 0xbda88870, 0xbd1b667e, 0xbe4f9ceb, 0x3d854643, 0x3d8559d7}},
    {"44100 aa up3 lin", "rate 44100\nch 2\nparams 3 250 1 1 7 0 -1310443107 1072909783 1 0 0 1094987768\nreset\nproc 100000\n", 0xdfa7637e84d4bb00ull, 200000, {0x00000000, 0x00000000, 0xbe802c07, 0xbe8018df, 0xbd2dc19c, 0x3efd7abe, 0xbd6d2daf, 0xbd6d04c0}},
    {"44100 aa on, no preserve", "rate 44100\nch 2\nparams 5 0 2 0 4 0 150006878 1073044353 1 0 0 1094987768\nreset\nproc 100000\n", 0xc65d0d30955a95f3ull, 200000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xbe8d3eb8, 0xbd2e4e61, 0xbe72cce8, 0xbe72a52e}},
    {"44100 no preserve", "rate 44100\nch 2\nparams 5 0 2 0 4 0 150006878 1073044353 0 0 0 1094987768\nreset\nproc 100000\n", 0xecf7aca62c0119deull, 200000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x3ef0c5dd, 0xbe02c571, 0xbeb6a113, 0xbeb68d21}},
    {"44100 fifth up preset", "rate 44100\nch 2\nparams 7 0 2 0 0 0 -680996944 1073215760 0 0 0 1094987768\nreset\nproc 100000\n", 0xbac9f860e17c94ceull, 200000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xbc1acbba, 0x3e51f77c, 0xbba9c863, 0xbba89b8b}},
    {"44100 updates", "rate 44100\nch 2\nparams 7 0 2 1 0 0 -680996944 1073215760 0 0 0 1094987768\nreset\nproc 30000\nparams -4 0 2 1 0 0 -1522700739 1072260606 0 0 0 1094987768\nupdate\nproc 30000\nparams 3 200 2 1 0 0 774102750 1072906135 0 0 0 1094987768\nupdate\nproc 30000\n", 0x0fa0ebee26b80c90ull, 180000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xbc1acbba, 0x3b8f3ffd, 0xbde8d7aa, 0xbde88722}},
    {"44100 resets", "rate 44100\nch 2\nparams 5 0 2 1 2 0 150006878 1073044353 0 0 0 1094987768\nreset\nproc 30000\nparams -2 0 2 1 9 0 122601769 1072464446 0 0 0 1094987768\nreset\nproc 30000\nparams -9 120 2 1 9 0 1847585399 1071851743 0 0 0 1094987768\nupdate\nproc 30000\nparams 0 0 2 1 0 0 0 1072693248 0 0 0 1094987768\nreset\nproc 20000\n", 0xf95616d6e2a4c093ull, 220000, {0x00000000, 0x00000000, 0xbd955020, 0xbd95001d, 0x3e8ab26c, 0x3f037eac, 0x3d000000, 0x3d002800}},
    {"48000 up7", "rate 48000\nch 2\nparams 7 0 2 1 0 0 -680996944 1073215760 0 0 0 1094987768\nreset\nproc 100000\n", 0x245bacd55678baf3ull, 200000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xbe91a967, 0xbc964b20, 0xbd40aec7, 0xbd408a7f}},
    {"48000 down5", "rate 48000\nch 2\nparams -5 0 2 1 0 0 -680996944 1072167184 0 0 0 1094987768\nreset\nproc 100000\n", 0xc74ddc1180ab9a48ull, 200000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x3da5ade7, 0x3de69cbf, 0xbdbc86b0, 0xbdbc35da}},
    {"48000 cents lin", "rate 48000\nch 2\nparams 3 250 1 1 7 0 -1310443107 1072909783 0 0 0 1094987768\nreset\nproc 100000\n", 0xff134fcff29dff67ull, 200000, {0x00000000, 0x00000000, 0x3c9930d0, 0x3c9a70d0, 0xbb0d9153, 0xbee874ee, 0xbd9d00b5, 0xbd9cece4}},
    {"48000 acc3 mode12", "rate 48000\nch 2\nparams -3 -250 3 1 12 0 -740305482 1072334300 0 0 0 1094987768\nreset\nproc 100000\n", 0xf3b0212caebd9661ull, 200000, {0x00000000, 0x00000000, 0xbdb6f9c6, 0xbdb6aa39, 0x3d2d61a7, 0xbec20d06, 0xbe423fa2, 0xbe4217ab}},
    {"48000 drums up12", "rate 48000\nch 2\nparams 12 0 2 1 18 0 0 1073741824 0 0 0 1094987768\nreset\nproc 100000\n", 0x1c9de0cc2d30efa7ull, 200000, {0x00000000, 0x00000000, 0xbec51600, 0xbec50200, 0x3dce3c00, 0xbe43fc00, 0x3bf12000, 0x3bf26000}},
    {"48000 speech down3", "rate 48000\nch 2\nparams -3 0 2 1 8 0 -1722100819 1072359583 0 0 0 1094987768\nreset\nproc 100000\n", 0xe6cd2106eceb168dull, 200000, {0x00000000, 0x00000000, 0xbd978182, 0xbd973182, 0xb90c1a86, 0x3ef325d6, 0x3b5a4b60, 0x3b5ca4cf}},
    {"48000 low aa flag", "rate 48000\nch 2\nparams -7 0 2 1 3 0 150006878 1071995777 1 0 0 1094987768\nreset\nproc 100000\n", 0x9cb4dbdad9b7ddeaull, 200000, {0x00000000, 0x00000000, 0x3e1f7fea, 0x3e1fa7ea, 0x3e9dbd4d, 0xbdb018fc, 0xbd6e93ce, 0xbd6e6b45}},
    {"48000 aa up7", "rate 48000\nch 2\nparams 7 0 2 1 0 0 -680996944 1073215760 1 0 0 1094987768\nreset\nproc 100000\n", 0x8bb3683f0ee10299ull, 200000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xbdab38db, 0x3df2f25c, 0x3ca775e2, 0x3ca7c2cd}},
    {"48000 aa up12 acc3", "rate 48000\nch 2\nparams 12 0 3 1 12 0 0 1073741824 1 0 0 1094987768\nreset\nproc 100000\n", 0x959ad8f11c65d818ull, 200000, {0x00000000, 0x00000000, 0x3c7a4b81, 0x3c7cbdc9, 0xbe441c9d, 0x3f087a12, 0xbcc39514, 0xbcc34787}},
    {"48000 aa up3 lin", "rate 48000\nch 2\nparams 3 250 1 1 7 0 -1310443107 1072909783 1 0 0 1094987768\nreset\nproc 100000\n", 0xe212a2bd03117a56ull, 200000, {0x00000000, 0x00000000, 0x3e498447, 0x3e49aa9b, 0x3cce6d47, 0xbeae831f, 0xbd329257, 0xbd326b02}},
    {"48000 aa on, no preserve", "rate 48000\nch 2\nparams 5 0 2 0 4 0 150006878 1073044353 1 0 0 1094987768\nreset\nproc 100000\n", 0xaa890eb88b98a7f7ull, 200000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xbdfb4821, 0xbdbb1f1a, 0xbdfc19f8, 0xbdfbc8e8}},
    {"48000 no preserve", "rate 48000\nch 2\nparams 5 0 2 0 4 0 150006878 1073044353 0 0 0 1094987768\nreset\nproc 100000\n", 0xeb9e767e5b0bc9a3ull, 200000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xbe2c8010, 0x3dd79e60, 0xbdea8805, 0xbdea37db}},
    {"48000 fifth up preset", "rate 48000\nch 2\nparams 7 0 2 0 0 0 -680996944 1073215760 0 0 0 1094987768\nreset\nproc 100000\n", 0x245bacd55678baf3ull, 200000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xbe91a967, 0xbc964b20, 0xbd40aec7, 0xbd408a7f}},
    {"48000 resets", "rate 48000\nch 2\nparams 5 0 2 1 2 0 150006878 1073044353 0 0 0 1094987768\nreset\nproc 30000\nparams -2 0 2 1 9 0 122601769 1072464446 0 0 0 1094987768\nreset\nproc 30000\nparams -9 120 2 1 9 0 1847585399 1071851743 0 0 0 1094987768\nupdate\nproc 30000\nparams 0 0 2 1 0 0 0 1072693248 0 0 0 1094987768\nreset\nproc 20000\n", 0x35af28b16e56a598ull, 220000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x3e918ed5, 0x3e13e2d7, 0xbdf14400, 0xbdf13000}},
    {"44100 aa updates", "rate 44100\nch 2\nparams 7 0 2 1 0 0 -680996944 1073215760 1 0 0 1094987768\nreset\nproc 30000\nparams 5 0 2 1 0 0 150006878 1073044353 1 0 0 1094987768\nupdate\nproc 30000\n", 0xeb9a8bc295233670ull, 120000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x3dc5a0f6, 0x3d8cb9d4, 0x3d664fc6, 0x3d6675bd}},
};

int main()
{
    int bad = vegfx::checkCases("reno.pitchshift", kCases, (int)(sizeof kCases / sizeof kCases[0]));
    // the delay the plug-in reports follows the frame length of the mode and the step of the resampler
    using namespace reno::vegpack1;
    int32_t s[12] = {7, 0, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    const double r = std::pow(2.0, 7.0 / 12.0);
    std::memcpy(s + 6, &r, 8);
    const int l44 = pitchLatencyFor(s, 44100), l48 = pitchLatencyFor(s, 48000);
    const bool lok = l44 > 1024 && l48 > l44;
    std::printf("%s pitch latency: %d %d\n", lok ? "PASS" : "FAIL", l44, l48);
    if (!lok) ++bad;
    std::printf("%s\n", bad ? "FAILED" : "ALL PASSED");
    return bad ? 1 : 0;
}
