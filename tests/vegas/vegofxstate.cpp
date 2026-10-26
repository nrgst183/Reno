// vegofxstate: the effects of the sample projects through Reno's OpenFX host and back into a .veg.
//
//   vegofxstate <SlopFX.ofx> [folder with .veg files (default $RENO_SAMPLES)] [folder with VST2 plug-ins (default $RENO_TEST_VST_DIR)]
//
// Every effect Reno can host is made live, its settings are put into it (loadState) and taken out again before saving
// (saveState). The record written from that state must be the bytes VEGAS wrote, and after a setting is changed the
// record must read back with the new value. SlopFX's effects are hosted here (built from source for Linux); the samples
// use them a lot. The same for the VST2 audio effects: the program written from what Reno's VST2 host saved must be
// the one VEGAS wrote.
#include "../../src/vegas/vegas.h"
#include "../../src/vegas/vegformat.h"
#include "../../src/vegas/vegorigin.h"
#include "../../src/ofx/ofxhost.h"
#include "../../src/audiofx/audioplugin.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include "../testdata.h"

using namespace reno;

static int g_fail = 0, g_pass = 0;
static void check(bool ok, const std::string& what)
{
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL: %s\n", what.c_str()); }
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: vegofxstate SlopFX.ofx [folder]\n"); return 2; }
    const std::string dir = argc > 2 ? argv[2] : testDataDir("RENO_SAMPLES");
    if (testDataMissing(dir)) return 0;
    std::string err;
    if (!ofx::Host::get().loadBinary(argv[1], &err)) { std::printf("could not load %s: %s\n", argv[1], err.c_str()); return 1; }
    std::vector<std::string> files;
    for (const auto& e : std::filesystem::recursive_directory_iterator(dir))
        if (e.is_regular_file() && endsWithNoCase(e.path().string(), ".veg")) files.push_back(e.path().string());
    std::sort(files.begin(), files.end());
    int hosted = 0, same = 0, changed = 0, added = 0;
    std::map<std::string, int> perPlugin;
    for (const auto& f : files) {
        Project p;
        if (!importVegasProject(f, p).ok) continue;
        p.forEachFx([&](FxSlot& s) {
            if (s.kind != "ofx") return;
            ofx::Plugin* pl = ofx::Host::get().findPlugin(s.pluginId);
            if (!pl || !pl->load()) return;
            const std::string ctx = pl->filterContext().empty() ? std::string(kOfxImageEffectContextGenerator) : pl->filterContext();
            ofx::InstanceSetup setup;
            std::unique_ptr<ofx::Instance> inst = pl->createInstance(ctx, setup, &err);
            if (!inst) return;
            const auto* o = vegOriginAs<VegFxOrigin>(s.veg, VegOrigin::FxK);
            if (!o) return;
            ++hosted;
            ++perPlugin[s.pluginId];
            inst->loadState(s.state);
            const Json state = inst->saveState();
            // the record from what the host saved
            const veg::Bytes orig = o->chunk->data.sub(o->entryOff, o->entryLen);
            veg::OfxWriteInput in;
            in.rawId = "{Svfx:" + s.pluginId + "}";
            in.preset = orig.text(orig.u32(0) + orig.u32(0x28), orig.u32(0x2c));
            in.flags = orig.u32(0x20);
            in.state = state;
            in.original = orig;
            const veg::OfxWriteResult r = veg::writeOfxRecord(in);
            bool ident = r.record == std::string((const char*)orig.data(), orig.size());
            same += ident ? 1 : 0;
            if (!ident) {
                // settings the plug-in has that it did not have when VEGAS saved the project are added (VEGAS
                // would add them too); everything else must be as it was
                veg::OfxRecord a, b;
                veg::readOfxRecord(orig, 0, a);
                veg::readOfxRecord(veg::Bytes((const uint8_t*)r.record.data(), r.record.size()), 0, b);
                bool onlyAdded = true;
                for (const auto& m : a.state.get("params").members())
                    onlyAdded = onlyAdded && b.state.get("params").get(m.first).dump() == m.second.dump();
                if (onlyAdded) { ++added; ident = true; }
            }
            check(ident, f + ": '" + s.pluginId + "' after the host: the same record (or the same with settings added)");
            if (!ident) {
                // which settings differ
                veg::OfxRecord a, b;
                veg::readOfxRecord(orig, 0, a);
                veg::readOfxRecord(veg::Bytes((const uint8_t*)r.record.data(), r.record.size()), 0, b);
                for (const auto& m : b.state.get("params").members())
                    if (!a.state.get("params").has(m.first)) std::printf("   added: %s = %s\n", m.first.c_str(), m.second.dump().c_str());
                    else if (a.state.get("params").get(m.first).dump() != m.second.dump())
                        std::printf("   changed: %s: %s -> %s\n", m.first.c_str(), a.state.get("params").get(m.first).dump().substr(0, 200).c_str(),
                                    m.second.dump().substr(0, 200).c_str());
                std::printf("   params %zu -> %zu, unread %d / %d\n", a.state.get("params").size(), b.state.get("params").size(), a.unreadParams, b.unreadParams);
            }
            check(r.dropped.empty(), s.pluginId + ": nothing left out");
            // a setting changed in the host: the record holds it
            for (const auto& up : inst->params.params) {
                ofx::Param* prm = up.get();
                if (!prm->hasValue() || prm->dim != 1 || prm->isWhole() || !prm->keys.empty()) continue;
                prm->value[0] = prm->value[0] * 0.5 + 0.125;
                const double want = prm->value[0];
                in.state = inst->saveState();
                const veg::OfxWriteResult r2 = veg::writeOfxRecord(in);
                veg::OfxRecord back;
                const bool ok = veg::readOfxRecord(veg::Bytes((const uint8_t*)r2.record.data(), r2.record.size()), 0, back);
                const Json& e = back.state.get("params").get(prm->name);
                check(ok && std::fabs(e.get("v").at(0).asDouble() - want) < 1e-12, s.pluginId + ": a changed setting (" + prm->name + ") is in the record");
                ++changed;
                break;
            }
        });
    }
    std::printf("effects hosted: %d, records the same after the host: %d, the same with settings the plug-in has gained: %d, changed settings written: %d\n",
                hosted, same, added, changed);

    // VST2: the plug-ins in the folder, by their id
    const std::string vstDir = argc > 3 ? argv[3] : testDataDir("RENO_TEST_VST_DIR");
    std::map<std::string, AudioPluginDesc> vsts;
    std::error_code vstEc;
    if (!std::filesystem::is_directory(vstDir, vstEc)) std::printf("SKIP: VST2 part (no folder %s)\n", vstDir.c_str());
    else
    for (const auto& e : std::filesystem::directory_iterator(vstDir))
        for (const auto& d : describeAudioPluginFile(e.path().string(), &err))
            if (d.format == "VST2") vsts[d.uid] = d;
    int vstHosted = 0, vstSame = 0;
    for (const auto& f : files) {
        Project p;
        if (!importVegasProject(f, p).ok) continue;
        p.forEachFx([&](FxSlot& s) {
            if (s.kind != "audio" || s.audio.format != "VST2" || !s.state.isString()) return;
            auto it = vsts.find(s.audio.uid);
            const auto* o = vegOriginAs<VegFxOrigin>(s.veg, VegOrigin::FxK);
            if (it == vsts.end() || !o) return;
            std::unique_ptr<AudioPlugin> pl = createAudioPlugin(it->second, &err);
            if (!pl) return;
            ++vstHosted;
            pl->loadState(s.state.asString());
            const std::string st = pl->saveState();
            // the program VEGAS wrote: the entry's data without its 0xFFFFFFFF
            const veg::Bytes entry = o->chunk->data.sub(o->entryOff, o->entryLen);
            const uint32_t nl = entry.u32(20);
            const size_t q = 24 + (size_t)nl + 16;
            const uint32_t dl = entry.u32(q);
            const veg::Bytes prog = entry.sub(q + 8, dl - 4);
            std::string mine;
            const bool ok = veg::vstProgram(st, (uint32_t)std::stoul(s.audio.uid), prog, mine);
            const bool ident = ok && mine == std::string((const char*)prog.data(), prog.size());
            vstSame += ident ? 1 : 0;
            check(ident, f + ": VST '" + s.name + "' after the host: the same program");
            static int shown = 0;
            if (!ident && shown++ < 2) {
                auto hex = [](const std::string& b) { std::string h; for (unsigned char c : b) h += strfmt("%02x", c); return h; };
                std::printf("   VEGAS: %s\n   Reno:  %s\n   state in %s\n   state out %s\n", hex(std::string((const char*)prog.data(), prog.size())).c_str(), hex(mine).c_str(),
                            s.state.asString().c_str(), st.c_str());
            }
        });
    }
    std::printf("VST effects hosted: %d, programs the same after the host: %d\n", vstHosted, vstSame);
    for (const auto& kv : perPlugin) std::printf("  %-40s %d\n", kv.first.c_str(), kv.second);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
