// Lists what Reno's built-in OpenFX effects describe (each context of each effect: every setting with its type, label, hint, group, defaults, ranges,
// display ranges, steps and choices, in the order they are defined) as tab separated lines, for tools/ofxparams/vfxcheck.py to compare with VEGAS's
// resource files and with the ranges VEGAS's own plug-ins gave when they were run.
//
//   vfxparams_dump <output file>
#include "../../src/ofx/builtins.h"
#include "../../src/ofx/ofxhost.h"
#include <cstdio>
#include <cmath>

using namespace reno;
using namespace reno::ofx;

static std::string clean(std::string s)
{
    for (char& c : s)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}

int main(int argc, char** argv)
{
    FILE* f = std::fopen(argc > 1 ? argv[1] : "vfxparams.tsv", "w");
    if (!f) return 2;
    Host& host = Host::get();
    Binary* bin = host.loadBinary(kBuiltinBinaryPath, nullptr);
    if (!bin) return 3;
    for (const auto& up : bin->plugins) {
        Plugin* pl = up.get();
        if (!pl->load(nullptr)) continue;
        for (const char* c : {"Filter", "Generator", "Transition", "General"}) {
            const std::string full = std::string("OfxImageEffectContext") + c;
            if (!pl->supportsContext(full)) continue;
            InstanceSetup setup;
            setup.width = 64;
            setup.height = 36;
            std::string err;
            std::unique_ptr<Instance> inst = pl->createInstance(full, setup, &err);
            if (!inst) continue;
            std::fprintf(f, "EFFECT\t%s\t%s\t%s\t%s\n", pl->info.id.c_str(), c, clean(pl->info.label).c_str(), clean(pl->info.grouping).c_str());
            int order = 0;
            for (const auto& pp : inst->params.params) {
                const Param* p = pp.get();
                std::string vals, mins, maxs, dmins, dmaxs;
                auto num = [](double v) {
                    char b[64];
                    std::snprintf(b, sizeof b, "%.10g", v);
                    return std::string(b);
                };
                for (int k = 0; k < p->dim; ++k) {
                    vals += (k ? "," : "") + num((size_t)k < p->value.size() ? p->value[(size_t)k] : 0.0);
                    mins += (k ? "," : "") + num(p->props.getDouble(kOfxParamPropMin, k, -1.7976931348623157e308));
                    maxs += (k ? "," : "") + num(p->props.getDouble(kOfxParamPropMax, k, 1.7976931348623157e308));
                    dmins += (k ? "," : "") + num(p->props.getDouble(kOfxParamPropDisplayMin, k, -1.7976931348623157e308));
                    dmaxs += (k ? "," : "") + num(p->props.getDouble(kOfxParamPropDisplayMax, k, 1.7976931348623157e308));
                }
                std::string ch;
                for (const std::string& s : p->choices()) ch += (ch.empty() ? "" : "|") + s;
                const std::string cols[] = {std::to_string(order++), p->name, p->type, clean(p->label()), clean(p->hint()), p->parent(), p->secret() ? "1" : "0", p->enabled() ? "1" : "0",
                                            vals, mins, maxs, dmins, dmaxs, num(p->props.getDouble(kOfxParamPropIncrement, 0, 0)), num(p->props.getInt(kOfxParamPropDigits, 0, -1)),
                                            clean(p->props.getString(kOfxParamPropDoubleType)), clean(ch)};
                std::fputs("P", f);
                for (const std::string& col : cols) std::fprintf(f, "\t%s", col.c_str());
                std::fputc('\n', f);
            }
        }
    }
    std::fclose(f);
    return 0;
}
