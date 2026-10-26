// vegofx_wine: the Windows half of vegcookie_test. VEGAS Pro 18's own OpenFX effects (Cookie Cutter and any other stock
// effect) run through Reno's OpenFX host, built for Windows and run under Wine (vegofx_wine.sh).
//
//   vegofx_wine.exe <file.ofx> <jobs.json> <output folder>
//
// jobs.json (written by vegcookie_test --jobs): {"describe": [plug-in ids], "jobs": [{"name", "plugin", "state", "w", "h",
// "time", "pattern"}]}. For each plug-in in "describe", <output>/<id>.describe.json gets what the effect declares (every
// parameter's type, dimension, default, range and choices, the effect's and its clips' properties, and its answer to the
// clip preferences action). For each job the effect is made as a filter on a w x h frame, given the state (as Reno's
// .veg reader made it) and a test picture ("pattern": "bars" - opaque colour bars and a grey ramp, "white" - opaque
// white, "half" - the bars at half alpha, premultiplied), rendered at 'time' (frames), and the result written to
// <output>/<name>.pam (8-bit RGBA, top row first, premultiplied if the host's convention is right). Each job is
// rendered again with the effect told there is one processor: the pictures must be the same.
#include "../../src/ofx/ofxhost.h"
#include "../../src/ofx/vegas_ext.h"
#include <cstdio>
#include <cstring>

using namespace reno;
using namespace reno::ofx;

static Json propsJson(const PropertySet& s)
{
    Json o = Json::object();
    for (const auto& n : s.names()) {
        const Property* p = s.find(n);
        if (!p) continue;
        Json a = Json::array();
        switch (p->type) {
        case PType::Int: for (int v : p->i) a.push((double)v); break;
        case PType::Double: for (double v : p->d) a.push(v); break;
        case PType::String: for (const auto& v : p->s) a.push(v); break;
        case PType::Pointer: for (void* v : p->p) a.push(v ? std::string("(pointer)") : std::string("(null)")); break;
        default: break;
        }
        o[n] = a;
    }
    return o;
}

static ImagePtr pattern(const std::string& kind, int w, int h)
{
    ImagePtr img = Image::make(w, h);
    static const uint8_t bars[8][3] = {{255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0}, {255, 0, 255}, {255, 0, 0}, {0, 0, 255}, {16, 16, 16}};
    for (int y = 0; y < h; ++y) {
        uint8_t* r = img->row(h - 1 - y);      // y counted from the top
        for (int x = 0; x < w; ++x) {
            uint8_t c[4] = {255, 255, 255, 255};
            if (kind != "white") {
                if (y < h * 3 / 4) {
                    const uint8_t* b = bars[x * 8 / w];
                    c[0] = b[0]; c[1] = b[1]; c[2] = b[2];
                } else {
                    c[0] = c[1] = c[2] = (uint8_t)(x * 255 / std::max(1, w - 1));
                }
            }
            if (kind == "half") { for (int k = 0; k < 3; ++k) c[k] = (uint8_t)((c[k] * 128 + 127) / 255); c[3] = 128; }
            r[x * 4 + 0] = c[2]; r[x * 4 + 1] = c[1]; r[x * 4 + 2] = c[0]; r[x * 4 + 3] = c[3];     // B G R A
        }
    }
    return img;
}

static bool writePam(const std::string& path, const Image& img8)
{
    std::string s = strfmt("P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", img8.w, img8.h);
    for (int y = img8.h - 1; y >= 0; --y) {
        const uint8_t* r = img8.row(y);
        for (int x = 0; x < img8.w; ++x) {
            const uint8_t px[4] = {r[x * 4 + 2], r[x * 4 + 1], r[x * 4 + 0], r[x * 4 + 3]};
            s.append((const char*)px, 4);
        }
    }
    return writeFile(path, s);
}

int main(int argc, char** argv)
{
    if (argc < 4) { std::printf("usage: vegofx_wine file.ofx jobs.json outdir\n"); return 2; }
    std::string err, text;
    if (!readFile(argv[2], text)) { std::printf("no %s\n", argv[2]); return 1; }
    Json jobs;
    if (!Json::parse(text, jobs, &err)) { std::printf("bad %s: %s\n", argv[2], err.c_str()); return 1; }
    // ("vegasThreading": false gives VEGAS's effects Reno's real multithread suite, to see what that did)
    if (jobs.has("vegasThreading")) Host::get().config.vegasThreading = jobs.get("vegasThreading").asBool();
    if (!Host::get().loadBinary(argv[1], &err)) { std::printf("could not load %s: %s\n", argv[1], err.c_str()); return 1; }
    const std::string out = argv[3];
    int done = 0, failed = 0;
    for (const auto& id : jobs.get("describe").items()) {
        Plugin* pl = Host::get().findPlugin(id.asString());
        if (!pl || !pl->load(&err)) { std::printf("FAIL: %s not there: %s\n", id.asString().c_str(), err.c_str()); ++failed; continue; }
        Json d = Json::object();
        d["id"] = pl->info.id;
        d["label"] = pl->info.label;
        Descriptor* desc = pl->describeInContext(pl->filterContext(), &err);
        if (!desc) { std::printf("FAIL: %s not described: %s\n", id.asString().c_str(), err.c_str()); ++failed; continue; }
        d["effect"] = propsJson(desc->props);
        Json clips = Json::object();
        for (const auto& c : desc->clips) clips[c->name] = propsJson(c->props);
        d["clips"] = clips;
        Json params = Json::array();
        for (const auto& p : desc->params.params) {
            Json e = Json::object();
            e["name"] = p->name;
            e["type"] = p->type;
            e["dim"] = p->dim;
            Json ch = Json::array();
            for (const auto& c : p->choices()) ch.push(c);
            e["choices"] = ch;
            e["props"] = propsJson(p->props);
            params.push(e);
        }
        d["params"] = params;
        // what it answers to the clip preferences action (premultiplication of its output among them)
        InstanceSetup setup;
        setup.width = 320;
        setup.height = 180;
        setup.vegasContext = kVegasContextEvent;
        std::unique_ptr<Instance> inst = pl->createInstance(pl->filterContext(), setup, &err);
        if (inst) {
            PropertySet o;
            o.setString(kOfxImageEffectPropPreMultiplication, "(left as the host set it: " kOfxImagePreMultiplied ")");
            const OfxStatus st = pl->call(kOfxImageEffectActionGetClipPreferences, inst.get(), nullptr, &o);
            d["clipPreferencesStatus"] = (double)st;
            d["clipPreferences"] = propsJson(o);
        }
        writeFile(pathJoin(out, id.asString() + ".describe.json"), d.dump(1));
        std::printf("described %s (%zu parameters)\n", id.asString().c_str(), desc->params.params.size());
    }
    for (const auto& j : jobs.get("jobs").items()) {
        const std::string name = j.get("name").asString();
        // how many processors the effect is told there are (the multithread suite's numCPUs); 0: all
        setProcessorLimit(j.has("threads") ? j.get("threads").asInt() : 0);
        Plugin* pl = Host::get().findPlugin(j.get("plugin").asString());
        if (!pl || !pl->load(&err)) { std::printf("FAIL: %s: no plug-in\n", name.c_str()); ++failed; continue; }
        if (pl->filterContext().empty()) { std::printf("(%s: not a filter - skipped)\n", name.c_str()); continue; }
        InstanceSetup setup;
        setup.width = j.get("w").asInt();
        setup.height = j.get("h").asInt();
        setup.fps = j.has("fps") ? j.get("fps").asDouble() : 30.0;
        setup.vegasContext = kVegasContextEvent;
        std::unique_ptr<Instance> inst = pl->createInstance(pl->filterContext(), setup, &err);
        if (!inst) { std::printf("FAIL: %s: not made: %s\n", name.c_str(), err.c_str()); ++failed; continue; }
        const std::string pat = j.get("pattern").asString();
        const int w = setup.width, h = setup.height;
        for (const auto& c : inst->inputClipNames()) inst->setClipSource(c, [pat, w, h](double) { return pattern(pat, w, h); });
        inst->loadState(j.get("state"));
        RenderOptions ro;
        ro.interactive = false;
        ImagePtr img = inst->render(j.get("time").asDouble(), ro, &err);
        if (!img) { std::printf("FAIL: %s: render: %s\n", name.c_str(), err.c_str()); ++failed; continue; }
        if (img->depth != PixelDepth::Byte) img = img->converted(PixelDepth::Byte);
        writePam(pathJoin(out, name + ".pam"), *img);
        // the same picture whatever number of processors the effect is told there are (VEGAS's own effects get VEGAS's
        // one-thread multithread suite; given real threads, Cookie Cutter drew its shape once per band of rows)
        if (!j.has("threads")) {
            setProcessorLimit(1);
            ImagePtr one = inst->render(j.get("time").asDouble() + 1e-9, ro, &err);
            setProcessorLimit(0);
            if (one && one->depth != PixelDepth::Byte) one = one->converted(PixelDepth::Byte);
            if (!one || one->byteSize() != img->byteSize() || std::memcmp(one->data.get(), img->data.get(), img->byteSize()) != 0) {
                std::printf("FAIL: %s: a different picture on one processor\n", name.c_str());
                ++failed;
            }
        }
        // what the host now has as the state (to check every value of the .veg reached the effect)
        writeFile(pathJoin(out, name + ".state.json"), inst->saveState().dump(1));
        ++done;
    }
    std::printf("vegofx_wine: %d rendered, %d failed\n", done, failed);
    return failed ? 1 : 0;
}
