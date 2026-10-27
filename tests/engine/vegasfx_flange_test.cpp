// Flange/Wah-wah, Reno's version of VEGAS's DirectX effect (sfppack3_x64.dll, vegasflange.h): the sound the engine makes bit for bit
// against what the plug-in's own code gave (expected hashes and samples were made with the DLL), the saved form, the presets,
// the parameters, and Reno's in-place use of the engine.
//
//   vegasfx_flange_test
#include "../../src/audiofx/vegasflange.h"
#include "vegasfx_pack3_common.h"

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

// "effect,dry,wet,rate,depth,center,resonance,bypass,period,multiplier,sync" (the plug-in's own units)
static vegflange::Settings parse(const char* s)
{
    vegflange::Settings st;
    int32_t* p = reinterpret_cast<int32_t*>(&st);
    std::sscanf(s, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", &p[0], &p[1], &p[2], &p[3], &p[4], &p[5], &p[6], &p[7], &p[8], &p[9], &p[10]);
    return st;
}

struct Case {
    const char* name;
    const char* a;              // the settings at the start
    const char* b;              // the settings from 'switchAt' on ("-": no change)
    int switchAt;               // frame
    int rate, block;
    const char* tempo;          // "-": the plug-in gets none, else "flags,bpm,numerator,denominator"
    uint64_t hash;
    uint32_t samples[4];        // output bits at frame 2000 and at the last frame, left and right
};
static const Case kCases[] = {
    {"def", "0,-800,0,10000,10,100,0,0,18,1000,0", "-", 0, 48000, 512, "-", 0xde085af66dd289abull, {0xbe9e7491u, 0xbe51d658u, 0xb49fd4cbu, 0xbe554dd0u}},
    {"p1", "1,-800,0,500,50,1000,50,0,18,1000,0", "-", 0, 48000, 512, "-", 0xa44ba53052f0cc43ull, {0x3dceca5eu, 0xbd4fe52eu, 0xb3f416fdu, 0x3e7d3712u}},
    {"p2", "1,-800,0,4500,50,1000,50,0,18,1000,0", "-", 0, 48000, 512, "-", 0x6b84a6b5d50b611dull, {0x3dd0e6eau, 0x3d158d45u, 0xb3f4496au, 0x3e7e3d4fu}},
    {"p3", "1,-800,0,10000,50,2000,40,0,18,1000,0", "-", 0, 48000, 512, "-", 0x6d773dada8ce171eull, {0x3e046f67u, 0x3d43aa40u, 0xb3b24f02u, 0x3e2b9568u}},
    {"w1", "2,-800,0,1000,50,1000,50,0,18,1000,0", "-", 0, 48000, 512, "-", 0x149efb722ffdfd37ull, {0xbdd8a4dau, 0x3e3ccc10u, 0xb3ea4f0eu, 0x3d26fbc5u}},
    {"w2", "2,-800,0,2000,50,1000,80,0,18,1000,0", "-", 0, 48000, 512, "-", 0xd8d32243e4c81f97ull, {0xbd500810u, 0x3d041c29u, 0xb198144cu, 0x3d5d0ee8u}},
    {"w3", "2,-150,0,200,50,500,100,0,18,1000,0", "-", 0, 48000, 512, "-", 0x4a96e49ee90244bbull, {0x3cd40159u, 0x3b952861u, 0xb8d2fe42u, 0x3cc5e35du}},
    {"w4", "2,-800,0,500,70,3000,50,0,18,1000,0", "-", 0, 48000, 512, "-", 0x6ae126426c8f6fa0ull, {0xbba81412u, 0x3d5b67d0u, 0x3420cf21u, 0x3c3f6c09u}},
    {"w5", "2,-800,0,10000,50,2000,50,0,18,1000,0", "-", 0, 48000, 512, "-", 0xe35404636ff47d79ull, {0xbc21a581u, 0x3c960fd5u, 0xb435b35bu, 0x3da4c72cu}},
    {"f1", "0,-50,-60,1000,85,100,0,0,18,1000,0", "-", 0, 48000, 512, "-", 0xfff234222fd457a0ull, {0x3e00825bu, 0xbc950fc3u, 0xb42e26a5u, 0x3d7fa10du}},
    {"fast", "0,-60,-60,5000,50,100,0,0,18,1000,0", "-", 0, 48000, 512, "-", 0x6171741a9e72cda9ull, {0x3c1aecfcu, 0x3d608ed9u, 0xb41c1cf6u, 0x3d583908u}},
    {"slow", "0,-30,-20,100,100,100,0,0,18,1000,0", "-", 0, 48000, 512, "-", 0x880a549144a2d5cbull, {0x3e14200fu, 0xbda7dfd5u, 0xb4853015u, 0x3e69eedfu}},
    {"slow2", "0,-25,-25,500,85,100,0,0,18,1000,0", "-", 0, 48000, 512, "-", 0x2d2052ee3aecef76ull, {0xbe137d71u, 0xbcc5827au, 0x3318435au, 0x3d4caa1bu}},
    {"warb1", "0,-50,-50,20000,85,100,0,0,18,1000,0", "-", 0, 48000, 512, "-", 0xe30dfdb01f0fe051ull, {0x3d59171fu, 0x3c01dcc0u, 0xb4295a71u, 0x3d3e5658u}},
    {"warb2", "0,-50,-50,20000,20,100,0,0,18,1000,0", "-", 0, 48000, 512, "-", 0x49300198b2dcd360ull, {0xbd9f1741u, 0x3c852d5au, 0xb479d811u, 0x3d55f91au}},
    {"mute", "0,0,-800,5000,60,100,50,0,18,1000,0", "-", 0, 48000, 512, "-", 0x9369fe8de6a5d722ull, {0x3d793a80u, 0x3d72cdd8u, 0xb43adfb0u, 0x3e53224fu}},
    {"drywet", "0,0,0,3000,100,100,50,0,18,1000,0", "-", 0, 48000, 512, "-", 0x9fd6556950cbdd20ull, {0x3eabb5cbu, 0x3e666785u, 0xb4d4d667u, 0x3ca127cdu}},
    {"wetonly", "0,-800,120,7000,100,100,50,0,18,1000,0", "-", 0, 48000, 512, "-", 0x563b5af68718355eull, {0x3da121deu, 0x400b68abu, 0xb6aff4b7u, 0xbf1c7fd3u}},
    {"p-low", "1,0,-100,1,1,100,1,0,18,1000,0", "-", 0, 48000, 512, "-", 0xc420a9c572ad8a25ull, {0x3dc78782u, 0x3d55c954u, 0xb449c30au, 0x3e579403u}},
    {"p-hi", "1,-800,0,20000,100,5000,100,0,18,1000,0", "-", 0, 48000, 512, "-", 0x4ecb91a9cd5b9b31ull, {0x3dfa2e1bu, 0x3d6485e0u, 0xb4392440u, 0x3e567586u}},
    {"w-hi", "2,-30,-30,20000,100,5000,100,0,18,1000,0", "-", 0, 48000, 512, "-", 0x4ab4c6f6b598e942ull, {0xbd33228cu, 0x3d401cd6u, 0xb406bfc3u, 0x3e10c376u}},
    {"w-lowd", "2,-800,0,200,1,500,1,0,18,1000,0", "-", 0, 48000, 512, "-", 0x8e3fffd61ab0e25cull, {0xbe1885e4u, 0x3d1200d1u, 0x33f91a84u, 0x3d0b8e2fu}},
    {"p-d99", "1,-800,0,2500,99,3333,99,0,18,1000,0", "-", 0, 48000, 512, "-", 0x384eb10cc137f93bull, {0x3da79f82u, 0x3d8a8641u, 0xb4747f3du, 0x3e524e77u}},
    {"w-d51", "2,-800,0,2500,51,3333,49,0,18,1000,0", "-", 0, 48000, 512, "-", 0x1dee8ed3e71367c3ull, {0x3cfdcf07u, 0x3cc186c4u, 0x3414adbau, 0x3c863a52u}},
    {"fsync18", "0,-800,0,1000,60,100,0,0,18,1000,1", "-", 0, 48000, 512, "1,120,4,4", 0x9913444d89d799b3ull, {0xbeb83d9fu, 0xbe4ed171u, 0x33240619u, 0xbd8b6259u}},
    {"fsync0", "0,-800,0,1000,60,100,0,0,0,1000,1", "-", 0, 48000, 512, "1,120,4,4", 0x9445e6b8e7a43b0bull, {0xbd80cad2u, 0xbe12dec5u, 0xb4456819u, 0xbde6dfb8u}},
    {"fsync13", "0,-800,0,1000,100,100,0,0,13,500,1", "-", 0, 48000, 512, "1,120,4,4", 0x69aeff8eefa0e1adull, {0xbda26428u, 0x3e237562u, 0xb4e7374fu, 0xbe4c6259u}},
    {"psync18", "1,-800,0,4500,50,1000,50,0,18,1000,1", "-", 0, 48000, 512, "1,120,4,4", 0xa44ba53052f0cc43ull, {0x3dceca5eu, 0xbd4fe52eu, 0xb3f416fdu, 0x3e7d3712u}},
    {"psync7", "1,-800,0,4500,50,1000,50,0,7,3333,1", "-", 0, 48000, 512, "1,120,4,4", 0x8eb8b68b16bebc94ull, {0x3e094c8du, 0x3ca3db1bu, 0xb3d6e3a8u, 0x3e583f93u}},
    {"wsync10", "2,-800,0,1000,70,1000,80,0,10,1,1", "-", 0, 48000, 512, "1,120,4,4", 0xc28dd0672d6f3af9ull, {0xff800000u, 0xff800000u, 0xffc00000u, 0xffc00000u}},
    {"wsync16", "2,-800,0,500,50,1000,60,0,16,2000,1", "-", 0, 48000, 512, "1,120,4,4", 0x97d0caffa23aac82ull, {0xbcd7d2a0u, 0x3e7efed9u, 0xb3945936u, 0xbe472fd6u}},
    {"fsync13 90.5 bpm 3/4", "0,-800,0,1000,100,100,0,0,13,500,1", "-", 0, 48000, 512, "5,90.5,3,4", 0x0f4e6999cca10884ull, {0x3ea23428u, 0x3e2725d9u, 0x34820f34u, 0xbe37c8feu}},
    {"psync7 no bpm", "1,-800,0,4500,50,1000,50,0,7,3333,1", "-", 0, 48000, 97, "7,77,7,8", 0xaa79c48f5e56e0fdull, {0x3e00c93cu, 0xbc26d79eu, 0xb3ea6334u, 0x3e73726du}},
    {"wsync16 no tempo", "2,-800,0,500,50,1000,60,0,16,2000,1", "-", 0, 48000, 512, "-", 0x97d0caffa23aac82ull, {0xbcd7d2a0u, 0x3e7efed9u, 0xb3945936u, 0xbe472fd6u}},
    {"change fl", "0,-800,0,10000,10,100,0,0,18,1000,0", "0,-50,-60,1000,85,100,0,0,18,1000,0", 8000, 48000, 512, "-", 0xce17c23bee3f4cb2ull, {0xbe9e7491u, 0xbe51d658u, 0xb3c185beu, 0x3e3fa8bbu}},
    {"change fl wah", "0,-50,-60,1000,85,100,0,0,18,1000,0", "2,-800,0,1000,50,1000,50,0,18,1000,0", 8000, 48000, 512, "-", 0x733678a6eb26eae7ull, {0x3e00825bu, 0xbc950fc3u, 0xb3e2b76bu, 0xbd8a13d3u}},
    {"change wah ph", "2,-150,0,200,50,500,100,0,18,1000,0", "1,-800,0,4500,50,1000,50,0,18,1000,0", 5000, 48000, 333, "-", 0x58b40d31761a0d26ull, {0x3cd40159u, 0x3b952861u, 0xb3e9070au, 0x3e713dbau}},
    {"change sync", "0,-800,0,1000,60,100,0,0,18,1000,1", "0,-800,0,1000,100,100,0,0,13,500,1", 4000, 48000, 500, "1,120,4,4", 0x6724d72938e30c2full, {0xbeb83d9fu, 0xbe4ed171u, 0xb4e7374fu, 0xbe4c6259u}},
    {"small blocks", "1,-800,0,333,77,444,66,0,18,1000,0", "-", 0, 48000, 97, "-", 0xc52c65926f259ccaull, {0x3d6566f4u, 0x3d8e36a3u, 0xb3c42ecdu, 0xbd5017ecu}},
    {"whole signal in one block", "2,-800,0,15000,30,800,20,0,18,1000,0", "-", 0, 48000, 24000, "-", 0x1bd49d207eb2563eull, {0xbe09f3beu, 0x3d1609ceu, 0x340a1b90u, 0x3d055bbdu}},
    {"def", "0,-800,0,10000,10,100,0,0,18,1000,0", "-", 0, 44100, 512, "-", 0x2e9e09a7981b4187ull, {0x3e99aa67u, 0xbe03e654u, 0x3451a26eu, 0xbe8965e0u}},
    {"p1", "1,-800,0,500,50,1000,50,0,18,1000,0", "-", 0, 44100, 512, "-", 0xd0255d0348caac2aull, {0x3de65749u, 0xbd0e3510u, 0xb40186c3u, 0x3e7b6053u}},
    {"p2", "1,-800,0,4500,50,1000,50,0,18,1000,0", "-", 0, 44100, 512, "-", 0x5e49ce468ebda9b5ull, {0x3d94f5fdu, 0x3d067216u, 0xb4007a33u, 0x3e3dfa48u}},
    {"p3", "1,-800,0,10000,50,2000,40,0,18,1000,0", "-", 0, 44100, 512, "-", 0x08446d2e15848e4full, {0x3e20a506u, 0x3d49a691u, 0xb4881459u, 0x3e48dec9u}},
    {"w1", "2,-800,0,1000,50,1000,50,0,18,1000,0", "-", 0, 44100, 512, "-", 0xaf0bba675a9d51a4ull, {0xbe05060fu, 0x3e0dd575u, 0xb3ea654du, 0x3d32ddf9u}},
    {"w2", "2,-800,0,2000,50,1000,80,0,18,1000,0", "-", 0, 44100, 512, "-", 0xd0a0388cae2a4205ull, {0xbd6d4784u, 0x3c4914ceu, 0xb3c6bb0au, 0x3e9e8342u}},
    {"w3", "2,-150,0,200,50,500,100,0,18,1000,0", "-", 0, 44100, 512, "-", 0xf5241769b73876faull, {0x3cc7041eu, 0x3b76e0e0u, 0xb8cba342u, 0xbc623631u}},
    {"w4", "2,-800,0,500,70,3000,50,0,18,1000,0", "-", 0, 44100, 512, "-", 0x802c6dc2f3a5ddddull, {0x3d0a5b47u, 0x3d4786d7u, 0x34241226u, 0x3c28acb8u}},
    {"w5", "2,-800,0,10000,50,2000,50,0,18,1000,0", "-", 0, 44100, 512, "-", 0x0aaf59a91141636eull, {0xbdbd679au, 0x3c8299b7u, 0x33fd954eu, 0x3c83cdaau}},
    {"f1", "0,-50,-60,1000,85,100,0,0,18,1000,0", "-", 0, 44100, 512, "-", 0x33c3fe7769a0754dull, {0xbcdb443au, 0xbafa1e26u, 0xb371744cu, 0x3d652bdfu}},
    {"fast", "0,-60,-60,5000,50,100,0,0,18,1000,0", "-", 0, 44100, 512, "-", 0x85793dc8a077136full, {0x3d7e7d0cu, 0x3d92d01bu, 0xb40113f6u, 0x3e072b0au}},
    {"slow", "0,-30,-20,100,100,100,0,0,18,1000,0", "-", 0, 44100, 512, "-", 0x7c653f33833423e2ull, {0x3e4a58fau, 0xbd1a36eeu, 0xb333d92cu, 0x3eb39177u}},
    {"slow2", "0,-25,-25,500,85,100,0,0,18,1000,0", "-", 0, 44100, 512, "-", 0x08a249e0db86a4d3ull, {0x3e153087u, 0xbd8525dbu, 0xb41eba6bu, 0x3e120cddu}},
    {"warb1", "0,-50,-50,20000,85,100,0,0,18,1000,0", "-", 0, 44100, 512, "-", 0x287b4c448ec77dbaull, {0x3e0b7db5u, 0x3c481f2du, 0xb3ac973du, 0x3de98955u}},
    {"warb2", "0,-50,-50,20000,20,100,0,0,18,1000,0", "-", 0, 44100, 512, "-", 0x5e0cfe37dd7032a3ull, {0xbd069d24u, 0x3baac41fu, 0xb3da3b67u, 0xbcd096bbu}},
    {"mute", "0,0,-800,5000,60,100,50,0,18,1000,0", "-", 0, 44100, 512, "-", 0x9369fe8de6a5d722ull, {0x3d793a80u, 0x3d72cdd8u, 0xb43adfb0u, 0x3e53224fu}},
    {"drywet", "0,0,0,3000,100,100,50,0,18,1000,0", "-", 0, 44100, 512, "-", 0xdf2a3b85e7c34097ull, {0x3eabfe09u, 0x3d9b2dd4u, 0xb28263d0u, 0x3e94e05du}},
    {"wetonly", "0,-800,120,7000,100,100,50,0,18,1000,0", "-", 0, 44100, 512, "-", 0x699460ffef81e884ull, {0x40a2cfb6u, 0x404d676bu, 0xb6a2fa2cu, 0x4096a013u}},
    {"p-low", "1,0,-100,1,1,100,1,0,18,1000,0", "-", 0, 44100, 512, "-", 0xafcdc059e25bd09eull, {0x3dc5dc50u, 0x3d5bef62u, 0xb44a4f4fu, 0x3e5c9f5eu}},
    {"p-hi", "1,-800,0,20000,100,5000,100,0,18,1000,0", "-", 0, 44100, 512, "-", 0x7b1dc6cea9e03427ull, {0x3d8f12c8u, 0x3d7a08a1u, 0xb480b5c3u, 0x3e5a36f3u}},
    {"w-hi", "2,-30,-30,20000,100,5000,100,0,18,1000,0", "-", 0, 44100, 512, "-", 0x1ea2bce81e73008dull, {0x3cf85612u, 0x3d21a7e0u, 0xb301a48fu, 0x3e0b722fu}},
    {"w-lowd", "2,-800,0,200,1,500,1,0,18,1000,0", "-", 0, 44100, 512, "-", 0x45fd4f3e462c2c1aull, {0xbe2d34efu, 0x3d06c91fu, 0x341a7836u, 0x3d00639cu}},
    {"p-d99", "1,-800,0,2500,99,3333,99,0,18,1000,0", "-", 0, 44100, 512, "-", 0x11f4e92cc3b3386cull, {0x3df6f278u, 0x3d57a49au, 0xb430fee9u, 0x3e571cb9u}},
    {"w-d51", "2,-800,0,2500,51,3333,49,0,18,1000,0", "-", 0, 44100, 512, "-", 0xfcc547d985ddc95aull, {0x3ce43015u, 0x3caca3abu, 0x341fc1d2u, 0x3c3d643eu}},
    {"fsync18", "0,-800,0,1000,60,100,0,0,18,1000,1", "-", 0, 44100, 512, "1,120,4,4", 0x79d75947ea38d50dull, {0xbe3796c6u, 0xbe2be04cu, 0xb497beaau, 0x3e97007bu}},
    {"fsync0", "0,-800,0,1000,60,100,0,0,0,1000,1", "-", 0, 44100, 512, "1,120,4,4", 0xe14101a79592fc96ull, {0xbd2ed5d2u, 0x3e1cfd21u, 0x334260d7u, 0x3ee5d7c8u}},
    {"fsync13", "0,-800,0,1000,100,100,0,0,13,500,1", "-", 0, 44100, 512, "1,120,4,4", 0x01f2c3b6ee4950d3ull, {0x3e8b8d9du, 0x3e03b431u, 0x32e85602u, 0xbdd899f5u}},
    {"psync18", "1,-800,0,4500,50,1000,50,0,18,1000,1", "-", 0, 44100, 512, "1,120,4,4", 0xd0255d0348caac2aull, {0x3de65749u, 0xbd0e3510u, 0xb40186c3u, 0x3e7b6053u}},
    {"psync7", "1,-800,0,4500,50,1000,50,0,7,3333,1", "-", 0, 44100, 512, "1,120,4,4", 0xe58a5699a1828f43ull, {0x3e036cecu, 0x3d0e1083u, 0xb4027aa4u, 0x3e6eebceu}},
    {"wsync10", "2,-800,0,1000,70,1000,80,0,10,1,1", "-", 0, 44100, 512, "1,120,4,4", 0x04973134e73261c9ull, {0xff800000u, 0xff800000u, 0xffc00000u, 0xffc00000u}},
    {"wsync16", "2,-800,0,500,50,1000,60,0,16,2000,1", "-", 0, 44100, 512, "1,120,4,4", 0xa369c28f7999ccf9ull, {0xbd36a747u, 0x3e50580eu, 0xb34f96dbu, 0xbe3f4202u}},
    {"fsync13 90.5 bpm 3/4", "0,-800,0,1000,100,100,0,0,13,500,1", "-", 0, 44100, 512, "5,90.5,3,4", 0x459219b55d6a2f1full, {0x3e6ccee2u, 0x3c952b6du, 0xb3e5f939u, 0x3d8f719du}},
    {"psync7 no bpm", "1,-800,0,4500,50,1000,50,0,7,3333,1", "-", 0, 44100, 97, "7,77,7,8", 0xf12cc4b72771381cull, {0x3e08a5d6u, 0x3c5f76dbu, 0xb3d02b56u, 0x3e4b7e23u}},
    {"wsync16 no tempo", "2,-800,0,500,50,1000,60,0,16,2000,1", "-", 0, 44100, 512, "-", 0xa369c28f7999ccf9ull, {0xbd36a747u, 0x3e50580eu, 0xb34f96dbu, 0xbe3f4202u}},
    {"change fl", "0,-800,0,10000,10,100,0,0,18,1000,0", "0,-50,-60,1000,85,100,0,0,18,1000,0", 8000, 44100, 512, "-", 0xc35984d0f5c26f84ull, {0x3e99aa67u, 0xbe03e654u, 0xb3c4757fu, 0x3e608d21u}},
    {"change fl wah", "0,-50,-60,1000,85,100,0,0,18,1000,0", "2,-800,0,1000,50,1000,50,0,18,1000,0", 8000, 44100, 512, "-", 0xde2b2763796c3a42ull, {0xbcdb443au, 0xbafa1e26u, 0xb3e52fc8u, 0xbc157664u}},
    {"change wah ph", "2,-150,0,200,50,500,100,0,18,1000,0", "1,-800,0,4500,50,1000,50,0,18,1000,0", 5000, 44100, 333, "-", 0xf261415403cab72dull, {0x3cc7041eu, 0x3b76e0e0u, 0xb388c73au, 0xbcb14fedu}},
    {"change sync", "0,-800,0,1000,60,100,0,0,18,1000,1", "0,-800,0,1000,100,100,0,0,13,500,1", 4000, 44100, 500, "1,120,4,4", 0x102d3c090bcccbc5ull, {0xbe3796c6u, 0xbe2be04cu, 0x32e85602u, 0xbdd899f5u}},
    {"small blocks", "1,-800,0,333,77,444,66,0,18,1000,0", "-", 0, 44100, 97, "-", 0xddb14ddb871c7f5cull, {0x3d5f7cfdu, 0x3d910a34u, 0xb3fc143du, 0x3e5b70e3u}},
    {"whole signal in one block", "2,-800,0,15000,30,800,20,0,18,1000,0", "-", 0, 44100, 24000, "-", 0x67c89d1a3fd8c1c1ull, {0xbe0d7687u, 0x3d14295bu, 0x323bb3c4u, 0x3d33022fu}},
};

// the engine over the signal as the DLL harness did: blocks, in place, setup again when the settings change
static std::vector<float> stream(const Case& c, const std::vector<float>& in)
{
    vegflange::Engine e;
    e.setSettings(parse(c.a));
    e.setFormat(c.rate);
    vegflange::Tempo tp;
    const vegflange::Tempo* tempo = nullptr;
    if (std::strcmp(c.tempo, "-") != 0) {
        unsigned fl = 0;
        double bpm = 0;
        int num = 0, den = 0;
        std::sscanf(c.tempo, "%u,%lf,%d,%d", &fl, &bpm, &num, &den);
        tp.haveBpm = (fl & 1) != 0; tp.bpm = bpm; tp.haveMeter = (fl & 4) != 0; tp.numerator = num; tp.denominator = den;
        tempo = &tp;
    }
    std::vector<float> d = in;
    const int n = (int)(in.size() / 2);
    bool switched = false;
    for (int k = 0; k < n; k += c.block) {
        int cnt = std::min(c.block, n - k);
        if (std::strcmp(c.b, "-") != 0 && !switched && k >= c.switchAt) { e.setSettings(parse(c.b)); switched = true; }
        e.process(&d[(size_t)k * 2], &cnt, tempo);
    }
    return d;
}

int main()
{
    const std::vector<float> in = p3test::signal(24000);
    for (const Case& c : kCases) {
        const std::string tag = std::string(c.name) + " at " + std::to_string(c.rate) + " Hz, blocks of " + std::to_string(c.block);
        const std::vector<float> out = stream(c, in);
        char buf[96];
        std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)p3test::hashBits(out));
        check(p3test::hashBits(out) == c.hash, "sound is bit-identical to the plug-in's (" + tag + ")", buf);
        const size_t at[4] = {2000 * 2, 2000 * 2 + 1, 23999 * 2, 23999 * 2 + 1};
        bool same = true;
        for (int k = 0; k < 4 && same; ++k) same = p3test::bitsOf(out[at[k]]) == c.samples[k];
        check(same, "stored samples match (" + tag + ")");
    }
    // the blocks the sound is given in do not matter
    {
        Case d = kCases[0];
        d.block = 77;
        check(stream(kCases[0], in) == stream(d, in), "the block size does not change the sound");
    }
    // saved form: [u32 length 0x40][the effect's GUID][0x2c bytes]
    {
        vegflange::Settings s = vegflange::presets()[6].s;
        const std::string st = vegflange::encode(s);
        std::string raw;
        check(st.compare(0, 2, "S:") == 0, "the saved form starts with S:");
        raw = base64Decode(st.substr(2));
        uint32_t len = 0;
        if (raw.size() >= 4) std::memcpy(&len, raw.data(), 4);
        // 32cf44d6-72e5-4794-a1bc-c8d4a7f51703 as a GUID in memory
        static const unsigned char guid[16] = {0xd6, 0x44, 0xcf, 0x32, 0xe5, 0x72, 0x94, 0x47, 0xa1, 0xbc, 0xc8, 0xd4, 0xa7, 0xf5, 0x17, 0x03};
        check(raw.size() == 4 + 16 + 0x2c && len == 0x40 && std::memcmp(raw.data() + 4, guid, 16) == 0, "length 0x40, the plug-in's GUID, 0x2c bytes of settings");
        vegflange::Settings t;
        check(vegflange::decode(st, &t) && std::memcmp(&t, &s, sizeof s) == 0 && vegflange::encode(t) == st, "the settings are read back and written byte for byte");
        int32_t w[11];
        std::memcpy(w, raw.data() + 20, sizeof w);
        check(w[0] == 2 && w[1] == -150 && w[2] == 0 && w[3] == 200 && w[4] == 50 && w[5] == 500 && w[6] == 100 && w[7] == 0 && w[8] == 18 && w[9] == 1000 && w[10] == 0,
              "fields are at the plug-in's offsets (Wah-wah 3)");
        AudioPluginDesc d;
        d.format = "BUILTIN";
        d.uid = "reno.flangewah";
        std::string err;
        auto p = builtinAudioCreate(d, &err);
        check(p && p->loadState(st) && p->saveState() == st, "the plug-in loads and saves the same form");
        check(p && !p->loadState("garbage") && !p->loadState("S:AAAA"), "other states are refused");
        check(p && p->loadState("reno-builtin-audio 1\nRate=3\n") && p->saveState() == vegflange::encode(vegflange::presets()[0].s), "a stand-in's old state loads as the defaults");
        vegflange::Settings wild;
        wild.effect = 9;
        wild.dryOut = -99999;
        wild.rate = 0;
        wild.depth = 500;
        wild.centerFreq = 5;
        wild.resonance = 0;
        wild.period = 40;
        wild.multiplier = 0;
        vegflange::decode(vegflange::encode(wild), &t);
        check(t.effect == 2 && t.dryOut == -800 && t.rate == 1 && t.depth == 100 && t.centerFreq == 100 && t.resonance == 1 && t.period == 18 && t.multiplier == 1,
              "out-of-range values are brought into range");
    }
    // parameters and presets
    {
        AudioPluginDesc d;
        d.format = "BUILTIN";
        d.uid = "reno.flangewah";
        std::string err;
        auto p = builtinAudioCreate(d, &err);
        static const char* const names[] = {"Effect", "Center Frequency", "Resonance", "Rate", "Depth", "Wet Out", "Dry Out", "Period", "Period Multiplier", "Tempo Sync"};
        bool ok = p && p->paramCount() == 10;
        for (int k = 0; k < 10 && ok; ++k) ok = p->paramName(k) == names[k];
        check(ok, "the plug-in's own ten parameters, in its order");
        check(p && p->programCount() == 15 && p->programName(0) == "Default all parameters" && p->programName(1) == "Phaser 1" && p->programName(6) == "Wah-wah 3" &&
                  p->programName(14) == "Warble 2",
              "VEGAS's 15 presets");
        vegflange::Settings q;
        check(p && p->setProgram(9) && vegflange::decode(p->saveState(), &q) && q.effect == 0 && q.dryOut == -50 && q.wetOut == -60 && q.rate == 1000 && q.depth == 85,
              "1 Hz flange: -5 dB dry, -6 dB wet, 1 Hz, depth 85");
        check(p && p->setProgram(0) && p->paramText(5) == "0.0 dB" && p->paramText(6) == "-Inf." && p->paramText(7) == "Measures" && p->paramText(0) == "Flange",
              "the default: wet 0 dB, dry -Inf., measures, Flange");
    }
    // Reno's in-place use is the engine's
    {
        const Case& c = kCases[4];      // Wah-wah 1 at 48000
        const std::vector<float> ref = stream(c, in);
        check(p3test::run("reno.flangewah", vegflange::encode(parse(c.a)), c.rate, in) == ref, "in place, in blocks of 512, the effect gives the engine's sound");
        check(p3test::run("reno.flangewah", vegflange::encode(parse(c.a)), c.rate, in, 77) == ref, "and in blocks of 77");
        vegflange::Settings u = parse(c.a);
        u.bypass = 1;
        check(p3test::run("reno.flangewah", vegflange::encode(u), 48000, in) == in, "bypass leaves the sound alone");
        // tempo sync is 120 beats per minute in 4/4
        Case e = kCases[0];
        e.a = "0,-800,0,1000,60,100,1,0,13,1000,1";
        e.tempo = "5,120,4,4";
        check(p3test::run("reno.flangewah", vegflange::encode(parse(e.a)), 48000, in) == stream(e, in), "tempo sync uses 120 beats per minute");
    }
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
