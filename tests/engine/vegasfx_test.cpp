// Tests of "reno_cli vegasfx" (src/ofx/fxcompare.cpp) and the choice between VEGAS's plug-ins and Reno's built-in
// versions (HostConfig::preferBuiltinVegasFx, honoured by FxRuntime).
//
//   vegasfx_test <standinfx.ofx>      (tests/ofx/standinfx.cpp: a plug-in with VEGAS's Invert identifier, standing in for the real one)
//
// Covers the preset reader, the PNG writer, and FxRuntime picking the stand-in or the built-in (carrying settings
// across, leaving non-VEGAS slots alone, not rebuilding when nothing changed).
#include "../../src/engine/fxruntime.h"
#include "../../src/ofx/builtins.h"
#include "../../src/ofx/fxcompare.h"
#include "../../src/ofx/probe.h"
#include <cstdio>
#include <cstring>

using namespace reno;

static int gFail = 0, gPass = 0;
static void check(bool ok, const std::string& what)
{
    if (ok) { ++gPass; std::printf("PASS %s\n", what.c_str()); }
    else { ++gFail; std::printf("FAIL %s\n", what.c_str()); }
}

static uint32_t crcOf(const uint8_t* p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t k = 0; k < n; ++k) {
        crc ^= p[k];
        for (int i = 0; i < 8; ++i) crc = (crc & 1) ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
    }
    return ~crc;
}
static uint32_t be32(const std::string& s, size_t at)
{
    return ((uint32_t)(uint8_t)s[at] << 24) | ((uint32_t)(uint8_t)s[at + 1] << 16) | ((uint32_t)(uint8_t)s[at + 2] << 8) | (uint8_t)s[at + 3];
}

static void testPresets()
{
    const auto presets = ofx::parsePresetPackage(ofx::builtinPresetPackage());
    check(presets.size() > 100, strfmt("the built-in preset package has %d presets", (int)presets.size()));
    const ofx::PresetEntry* grate = nullptr;
    for (const auto& p : presets)
        if (p.plugin == "com.vegascreativesoftware:cookiecutter" && p.name == "Grate") grate = &p;
    check(grate != nullptr, "Cookie Cutter's Grate preset is found");
    if (grate) {
        check(grate->context == "Filter", "  its context is Filter");
        double repeatX = -1, centerY = -1;
        for (const auto& v : grate->values) {
            if (v.name == "RepeatX" && !v.numbers.empty()) repeatX = v.numbers[0];
            if (v.name == "Center" && v.numbers.size() == 2) centerY = v.numbers[1];
        }
        check(repeatX == 10, "  RepeatX is 10");
        check(centerY == 0.5, "  Center has two numbers");
    }
    const std::string xml = "<OfxPresetPackage><OfxPreset plugin=\"a&amp;b\" context=\"Generator\" name=\"Say &quot;hi&quot;\">"
                            "<OfxParamTypeString name=\"Text\"><OfxParamValue>1 &lt; 2</OfxParamValue></OfxParamTypeString>"
                            "<OfxParamTypeRGBA name=\"C\"><OfxParamValue>0.5 0.25 1 1</OfxParamValue></OfxParamTypeRGBA></OfxPreset>"
                            "<OfxPreset plugin=\"x\" name=\"Empty\"></OfxPreset></OfxPresetPackage>";
    const auto two = ofx::parsePresetPackage(xml);
    check(two.size() == 2, "two presets read from text");
    if (two.size() == 2) {
        check(two[0].plugin == "a&b" && two[0].name == "Say \"hi\"" && two[0].context == "Generator", "  attributes are unescaped");
        check(two[0].values.size() == 2 && two[0].values[0].isText && two[0].values[0].text == "1 < 2", "  text value unescaped");
        check(two[0].values.size() == 2 && two[0].values[1].numbers.size() == 4 && two[0].values[1].numbers[1] == 0.25, "  colour has four numbers");
        check(two[1].values.empty(), "  a preset with no values");
    }
    check(ofx::parsePresetPackage("<OfxPresetPackage></OfxPresetPackage>").empty(), "an empty package has no presets");
    check(ofx::parsePresetPackage("garbage <OfxPreset name=").empty(), "bad text gives nothing and does not crash");
}

static void testPng()
{
    const int w = 37, h = 5;
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    for (size_t k = 0; k < rgb.size(); ++k) rgb[k] = (uint8_t)(k * 7);
    const std::string png = ofx::encodePng(w, h, rgb);
    check(png.size() > 8 && png.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0, "PNG signature");
    bool crcOk = true, ihdr = false, iend = false;
    std::string idat;
    size_t at = 8;
    while (at + 12 <= png.size()) {
        const uint32_t len = be32(png, at);
        const std::string type = png.substr(at + 4, 4);
        if (at + 12 + len > png.size()) { crcOk = false; break; }
        if (crcOf((const uint8_t*)png.data() + at + 4, 4 + len) != be32(png, at + 8 + len)) crcOk = false;
        if (type == "IHDR") ihdr = len == 13 && be32(png, at + 8) == (uint32_t)w && be32(png, at + 12) == (uint32_t)h && png[at + 16] == 8 && png[at + 17] == 2;
        if (type == "IDAT") idat += png.substr(at + 8, len);
        if (type == "IEND") iend = true;
        at += 12 + len;
    }
    check(crcOk, "every PNG chunk's CRC is right");
    check(ihdr, "the header has the size, 8 bits, RGB");
    check(iend && at == png.size(), "ends with IEND, nothing after");
    // the stored zlib stream: a header, blocks of (flag, len, ~len, bytes) and an Adler-32
    std::string raw;
    size_t z = 2;
    bool streamOk = idat.size() > 6;
    while (streamOk && z + 5 <= idat.size()) {
        const bool last = idat[z] & 1;
        const size_t len = (uint8_t)idat[z + 1] | ((size_t)(uint8_t)idat[z + 2] << 8);
        raw.append(idat, z + 5, len);
        z += 5 + len;
        if (last) break;
    }
    bool pixels = raw.size() == (size_t)h * (w * 3 + 1);
    for (int y = 0; pixels && y < h; ++y) {
        if (raw[(size_t)y * (w * 3 + 1)] != 0) pixels = false;
        if (std::memcmp(raw.data() + (size_t)y * (w * 3 + 1) + 1, rgb.data() + (size_t)y * w * 3, (size_t)w * 3) != 0) pixels = false;
    }
    uint32_t a = 1, b = 0;
    for (unsigned char c : raw) { a = (a + c) % 65521u; b = (b + a) % 65521u; }
    check(pixels, "the stored stream holds the pixels");
    check(z + 4 == idat.size() && be32(idat, z) == ((b << 16) | a), "the Adler-32 is right");
    // bigger than one stored block
    std::vector<uint8_t> big((size_t)300 * 200 * 3, 77);
    const std::string png2 = ofx::encodePng(300, 200, big);
    check(png2.size() > 180000, "a picture of more than one stored block");
}

static ofx::Instance* videoOf(FxRuntime& fx, uint64_t id) { return fx.video(id); }

static bool isBuiltin(ofx::Instance* inst) { return inst && inst->plugin && inst->plugin->binary && inst->plugin->binary->builtin; }

static ImagePtr renderOnce(ofx::Instance* inst)
{
    inst->setClipSource(kOfxImageEffectSimpleSourceClipName, [](double t) { return ofx::makeTestPattern(160, 90, t); });
    ofx::RenderOptions ro;
    ro.interactive = false;
    std::string err;
    return inst->render(10.0, ro, &err);
}

static void testSwitching(const std::string& standin)
{
    ofx::Host& host = ofx::Host::get();
    host.config.preferBuiltinVegasFx = false;
    host.applyConfig();
    std::string err;
    check(host.loadBinary(standin, &err) != nullptr, "the stand-in plug-in loads " + err);
    check(host.loadBinary(ofx::kBuiltinBinaryPath, nullptr) != nullptr, "the built-ins load");
    const std::string invert = "com.vegascreativesoftware:invert";

    Project p;
    p.settings.width = 160;
    p.settings.height = 90;
    FxSlot s;
    s.id = 1001;
    s.kind = "ofx";
    s.pluginId = invert;
    s.name = "Invert";
    p.videoOut.push_back(s);
    FxSlot other;       // not VEGAS's own: never touched by the preference
    other.id = 1002;
    other.kind = "ofx";
    other.pluginId = "reno.test.nothere";
    other.name = "Missing";
    p.videoOut.push_back(other);

    FxRuntime fx;
    fx.prepare(p);
    ofx::Instance* first = videoOf(fx, 1001);
    check(first != nullptr, "an instance is made for Invert");
    check(first && !isBuiltin(first), "  from VEGAS's (the stand-in) plug-in while the preference says so");
    if (!first) return;
    // a changed setting must survive the switch
    first->userSetValue(first->params.find("BlendAmount"), std::vector<double>{0.5}, 0.0);
    ImagePtr realPic = renderOnce(first);
    check(realPic != nullptr, "  and it renders");
    fx.prepare(p);
    check(videoOf(fx, 1001) == first, "  prepare() again with nothing changed keeps the instance");

    host.config.preferBuiltinVegasFx = true;
    fx.prepare(p);
    ofx::Instance* second = videoOf(fx, 1001);
    check(second != nullptr && isBuiltin(second), "with the preference set the instance comes from the built-in");
    check(second && second->params.find("BlendAmount") && std::fabs(second->params.find("BlendAmount")->value[0] - 0.5) < 1e-9, "  the setting was carried over (0.5)");
    ImagePtr builtinPic = second ? renderOnce(second) : nullptr;
    check(builtinPic != nullptr, "  and it renders");
    if (realPic && builtinPic && realPic->w == builtinPic->w) {
        long diff = 0;
        for (size_t k = 0; k < realPic->byteSize(); ++k) diff += std::abs((int)realPic->data.get()[k] - (int)builtinPic->data.get()[k]);
        check(diff > 0, "  the two draw different pictures (the stand-in adds an offset)");
    } else check(false, "  pictures to compare");
    fx.prepare(p);
    check(videoOf(fx, 1001) == second, "  prepare() again keeps the built-in instance");
    check(!fx.error(1002).empty() || videoOf(fx, 1002) == nullptr, "an effect that is not installed is still an error, not made from anything");

    host.config.preferBuiltinVegasFx = false;
    fx.prepare(p);
    ofx::Instance* third = videoOf(fx, 1001);
    check(third != nullptr && !isBuiltin(third), "back to VEGAS's: the stand-in's instance again");
    check(third && third->params.find("BlendAmount") && std::fabs(third->params.find("BlendAmount")->value[0] - 0.5) < 1e-9, "  with the setting carried back");

    // findVideoPlugin follows the preference too
    std::string e2;
    ofx::Plugin* pl = findVideoPlugin(invert, &e2);
    check(pl && !(pl->binary && pl->binary->builtin), "findVideoPlugin: VEGAS's by default");
    host.config.preferBuiltinVegasFx = true;
    pl = findVideoPlugin(invert, &e2);
    check(pl && pl->binary && pl->binary->builtin, "findVideoPlugin: the built-in with the preference");
    check(host.findPlugin(invert) == pl, "Host::findPlugin agrees");
    host.config.preferBuiltinVegasFx = false;
    fx.clear();
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: vegasfx_test <standinfx.ofx>\n"); return 2; }
    testPresets();
    testPng();
    testSwitching(argv[1]);
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
