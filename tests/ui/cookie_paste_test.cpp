// An event's layered effect chain through Copy / Paste, Paste Event Attributes, and saving as .veg and reopening: every
// effect in order with its settings, keyframes, on/off and pre-pan/crop place. Uses a sample project (read only; saved to
// a copy in the output folder): the one named on the command line or in $RENO_SAMPLE_VEG, else the .veg under
// $RENO_SAMPLES whose event has the longest effect chain. Without one the test SKIPs.
//
//   cookie_paste_test <output folder> [project.veg]
//
// AppState only, no windows. Prints PASS/FAIL lines and a summary; exits non-zero on failure.
#include "../../src/app/appstate.h"
#include "../../src/vegas/vegas.h"
#include "../../src/ofx/ofxhost.h"
#include "uitestlib.h"
#include "../testdata.h"
#include <QApplication>
#include <QDir>
#include <cstdio>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

AppState& st() { return AppState::get(); }

std::string chainText(const FxChain& c)
{
    std::string s;
    for (const FxSlot& f : c)
        s += f.pluginId + (f.bypass ? " (off)" : "") + (f.beforePanCrop ? " (pre)" : "") + "; ";
    return s;
}

// Whether the effect's setting of this name holds a value. VEGAS writes a 0 for groups too, which the effect state leaves out.
// True when the effect isn't here to ask.
bool holdsValue(const std::string& plugin, const std::string& name)
{
    ofx::Plugin* pl = ofx::Host::get().findPlugin(plugin);
    if (!pl || !pl->load()) return true;
    ofx::Descriptor* d = pl->describeInContext(pl->filterContext());
    const ofx::Param* p = d ? d->params.find(name) : nullptr;
    return !p || p->hasValue();
}

// Every number (and keyframe) of 'want' is in 'got' with the same value.
bool sameSettings(const FxChain& want, const FxChain& got, std::string* why)
{
    if (want.size() != got.size()) { *why = "count " + std::to_string(got.size()); return false; }
    for (size_t i = 0; i < want.size(); ++i) {
        const FxSlot& a = want[i];
        const FxSlot& b = got[i];
        if (a.pluginId != b.pluginId || a.bypass != b.bypass || a.beforePanCrop != b.beforePanCrop) { *why = "slot " + std::to_string(i); return false; }
        for (const auto& m : a.state.get("params").members()) {
            const Json& x = m.second;
            const Json& y = b.state.get("params").get(m.first);
            if (!b.state.get("params").has(m.first) && !holdsValue(a.pluginId, m.first)) continue;  // a group: no value
            for (const char* part : {"v", "k", "kc", "s"}) {
                if (!x.has(part)) continue;
                if (x.get(part).dump() != y.get(part).dump()) {
                    *why = a.pluginId + " " + m.first + " " + part + ": " + x.get(part).dump() + " / " + y.get(part).dump();
                    return false;
                }
            }
        }
    }
    return true;
}


}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("cookie_paste");
    // the project: named, or the sample with the longest effect chain on one event
    auto longestChain = [](const Project& pr) {
        size_t n = 0;
        for (const Track& t : pr.tracks) for (const Event& e : t.events) n = std::max(n, e.fx.size());
        return n;
    };
    std::string veg = testSampleVeg(args.size() > 2 ? args[2].toLocal8Bit().constData() : nullptr);
    if (veg.empty()) {
        size_t best = 0;
        for (const std::string& f : testVegFiles(qEnvironmentVariable("RENO_SAMPLES").toStdString())) {
            Project pr;
            std::string e;
            if (!loadProjectFile(f, pr, &e)) continue;
            if (longestChain(pr) > best) { best = longestChain(pr); veg = f; }
        }
    }
    if (veg.empty() || !QFileInfo(QString::fromStdString(veg)).exists()) { std::printf("SKIP: no sample project (set RENO_SAMPLES or RENO_SAMPLE_VEG, or pass the .veg)\nSUMMARY: 0 passed, 0 failed\n"); return 0; }
    QDir().mkpath(out);
    uitest::setup({}, {});
    AppState& app = st();
    std::string err;
    if (!app.open(veg, &err)) { check(false, "the sample project opens", err); return uitest::summary(); }
    const Project& p = app.project();
    // the event with the longest chain, and another event to paste onto
    uint64_t srcId = 0, dstId = 0, srcTrack = 0;
    size_t most = 0;
    for (const Track& t : p.tracks) for (const Event& e : t.events) if (e.fx.size() > most) { most = e.fx.size(); srcId = e.id; srcTrack = t.id; }
    for (const Track& t : p.tracks) for (const Event& e : t.events) if (!dstId && e.id != srcId && t.type == TrackType::Video) dstId = e.id;
    if (!srcId || !dstId) { std::printf("SKIP: the sample project has no event with effects and another video event\nSUMMARY: 0 passed, 0 failed\n"); return 0; }
    const FxChain chain = p.findEvent(srcId)->fx;
    check(!chain.empty(), "the chain as read: " + chainText(chain));

    // Copy and Paste
    app.setSelection({srcId});
    app.copySelection();
    app.currentTrack = srcTrack;
    app.setCursor(30.0);
    app.pasteAtCursor();
    uint64_t pastedId = 0;
    for (const Track& t : app.project().tracks)
        if (t.id == srcTrack)
            for (const Event& e : t.events)
                if (e.id != srcId) pastedId = e.id;
    std::string why;
    const Event* pasted = app.project().findEvent(pastedId);
    check(pasted != nullptr, "Paste makes a new event");
    if (pasted) {
        check(sameSettings(chain, pasted->fx, &why), "the pasted event has the whole chain, every setting and keyframe", why);
        bool fresh = true;
        for (size_t i = 0; i < pasted->fx.size() && i < chain.size(); ++i) fresh = fresh && pasted->fx[i].id != chain[i].id;
        check(fresh, "the pasted effects are effects of their own (new ids)");
    }

    // Paste Event Attributes onto the other event
    app.pasteEventAttributes({dstId}, AppState::AttrAll);
    const Event* dst = app.project().findEvent(dstId);
    check(dst && sameSettings(chain, dst->fx, &why), "Paste Event Attributes puts the whole chain on the other event", why);

    // saved and opened again
    const std::string saved = (out + "/pasted.veg").toStdString();
    check(app.save(saved, &err), "saved as a .veg", err);
    Project q;
    std::string err2;
    check(loadProjectFile(saved, q, &err2), "the saved .veg opens", err2);
    const Event* qs = q.findEvent(srcId);
    int matching = 0;
    for (const Track& t : q.tracks)
        for (const Event& e : t.events)
            if (sameSettings(chain, e.fx, &why)) ++matching;
    check(matching >= 3, "after saving and opening again, the original, the pasted event and the attributes' event have the whole chain (" +
                             std::to_string(matching) + ")", why);
    (void)qs;
    return uitest::summary();
}
