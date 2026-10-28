// Recording: see recording.h.
#include "recording.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace reno {

std::vector<RecordSource> planRecordSources(const Project& p, const std::set<uint64_t>& armed)
{
    std::vector<RecordSource> out;
    for (const Track& t : p.tracks) {
        if (t.type != TrackType::Audio || !armed.count(t.id)) continue;
        RecordInput in = t.recordInput;
        if (in.inputBus && !p.findInputBus(in.inputBus)) in.inputBus = 0;      // an input bus that is gone: the default input
        bool found = false;
        for (RecordSource& s : out)
            if (s.input.sameSource(in)) {
                s.tracks.push_back(t.id);
                found = true;
                break;
            }
        if (!found) {
            RecordSource s;
            s.input = in;
            s.tracks.push_back(t.id);
            out.push_back(s);
        }
    }
    return out;
}

void RecordRouter::setup(const std::vector<RecordSource>& sources, const Sink& sink)
{
    sources_ = sources;
    sink_ = sink;
}

std::vector<std::pair<std::string, int>> RecordRouter::devices() const
{
    std::vector<std::pair<std::string, int>> out;
    for (const RecordSource& s : sources_) {
        if (s.input.inputBus) continue;
        const int need = s.input.first + s.input.channels;
        bool found = false;
        for (auto& d : out)
            if (d.first == s.input.device) {
                d.second = std::max(d.second, need);
                found = true;
            }
        if (!found) out.push_back(std::make_pair(s.input.device, need));
    }
    return out;
}

void RecordRouter::pushDevice(const std::string& device, const float* x, int channels, int frames)
{
    if (!sink_ || frames <= 0 || channels <= 0) return;
    for (size_t i = 0; i < sources_.size(); ++i) {
        const RecordSource& s = sources_[i];
        if (s.input.inputBus || s.input.device != device) continue;
        a_.assign((size_t)frames, 0.f);
        b_.assign((size_t)frames, 0.f);
        const int c0 = s.input.first, c1 = s.input.first + 1;
        for (int k = 0; k < frames; ++k) {
            if (c0 < channels) a_[(size_t)k] = x[(size_t)k * channels + c0];
            if (s.input.channels == 2) b_[(size_t)k] = c1 < channels ? x[(size_t)k * channels + c1] : a_[(size_t)k];
        }
        sink_((int)i, a_.data(), s.input.channels == 2 ? b_.data() : nullptr, frames);
    }
}

void RecordRouter::pushInputBus(uint64_t id, const float* left, const float* right, int frames)
{
    if (!sink_ || frames <= 0) return;
    for (size_t i = 0; i < sources_.size(); ++i)
        if (sources_[i].input.inputBus == id) sink_((int)i, left, right, frames);
}

std::vector<uint64_t> recordTargetEvents(const Track& t, const std::set<uint64_t>& selected)
{
    std::vector<uint64_t> out;
    for (const Event& e : t.events)
        if (selected.count(e.id)) out.push_back(e.id);
    return out;
}

RecordSpan planRecordSpan(const Project& p, const std::set<uint64_t>& armed, const std::set<uint64_t>& selected, double cursor, double selStart,
                          double selEnd)
{
    RecordSpan r;
    r.start = cursor;
    double last = 0, first = 1e30;
    for (const Track& t : p.tracks) {
        if (t.type != TrackType::Audio || !armed.count(t.id)) continue;
        for (uint64_t id : recordTargetEvents(t, selected))
            if (const Event* e = p.findEvent(id)) {
                r.intoEvents = true;
                last = std::max(last, e->end());
                first = std::min(first, e->start);
            }
    }
    const bool sel = selEnd > selStart;
    if (sel) {
        // inside the selection; the cursor may start the recording later in it
        r.start = std::min(selEnd, std::max(selStart, cursor));
        r.stop = selEnd;
    } else if (r.intoEvents) {
        r.stop = last;
        if (cursor > first) r.start = first;        // recording into an event starts with it (the cursor may be earlier: a pre-roll)
    }
    return r;
}

namespace {

struct Pass {
    double fileAt;              // file time at timeline time 'timelineAt'
    double timelineAt;
};

// The loop passes of a recording that began at c.start: the first runs from there to the loop's end, the others over the loop.
std::vector<Pass> passesOf(const RecordCommit& c, double recordedLength)
{
    std::vector<Pass> out;
    if (!c.loop || c.loopEnd <= c.loopStart) {
        out.push_back(Pass{c.lag, c.start});
        return out;
    }
    const double first = std::max(0.0, c.loopEnd - c.start), loopLen = c.loopEnd - c.loopStart;
    out.push_back(Pass{c.lag, c.start});
    double at = first;
    while (at < recordedLength - 1e-6 && out.size() < 1000) {
        out.push_back(Pass{c.lag + at, c.loopStart});
        at += loopLen;
    }
    return out;
}

void setTakes(Event& e, std::vector<Take> all, int active)
{
    const Take t = all[(size_t)active];
    e.mediaId = t.mediaId;
    e.offset = t.offset;
    e.name = t.name;
    all.erase(all.begin() + active);
    e.takes = all;
    e.activeTake = active;
}

std::vector<Take> takesOf(const Event& e)
{
    const int at = std::max(0, std::min(e.activeTake, (int)e.takes.size()));
    std::vector<Take> all(e.takes.begin(), e.takes.begin() + at);
    Take active;
    active.mediaId = e.mediaId;
    active.offset = e.offset;
    active.name = e.name;
    all.push_back(active);
    all.insert(all.end(), e.takes.begin() + at, e.takes.end());
    return all;
}

}  // namespace

std::vector<uint64_t> commitRecording(Project& p, const RecordCommit& c)
{
    std::vector<uint64_t> made;
    for (const RecordCommit::Item& item : c.items) {
        Track* t = p.findTrack(item.track);
        if (!t) continue;
        const double recorded = std::max(0.0, item.length - c.lag);
        const std::vector<Pass> passes = passesOf(c, recorded);
        if (!item.events.empty()) {
            // punch-in / take recording: a take of each selected event for every pass that reached it
            for (uint64_t id : item.events) {
                Event* e = nullptr;
                for (Event& x : t->events)
                    if (x.id == id) e = &x;
                if (!e) continue;
                std::vector<Take> all = takesOf(*e);
                int added = 0;
                for (size_t k = 0; k < passes.size(); ++k) {
                    const double passEnd = k + 1 < passes.size() ? passes[k + 1].fileAt : item.length;
                    const double at = passes[k].fileAt + (e->start - passes[k].timelineAt);       // the file time of the event's start
                    if (at < passes[k].fileAt - 1e-6) continue;       // the recording began after the event did
                    if (at + 0.001 >= passEnd) continue;              // the pass ended before it got there
                    Take tk;
                    tk.mediaId = item.mediaId;
                    tk.offset = std::max(0.0, at);
                    tk.name = item.name;
                    all.push_back(tk);
                    ++added;
                }
                if (!added) continue;
                setTakes(*e, all, (int)all.size() - 1);
                made.push_back(e->id);
            }
            continue;
        }
        // a new event
        double at = c.start, len = recorded;
        if (c.loop && c.loopEnd > c.loopStart && passes.size() > 1) {
            at = c.loopStart;
            len = c.loopEnd - c.loopStart;
        }
        if (len <= 0) continue;
        Event e;
        e.id = p.newId();
        e.mediaId = item.mediaId;
        e.start = at;
        e.length = len;
        e.offset = c.lag;
        e.name = item.name;
        setDefaultFadeCurves(e, t->type);
        if (c.loop && passes.size() > 1) {
            // every pass is a take of the loop's event; the last is the active one
            std::vector<Take> all;
            for (size_t k = 0; k < passes.size(); ++k) {
                Take tk;
                tk.mediaId = item.mediaId;
                tk.offset = std::max(0.0, passes[k].fileAt + (at - passes[k].timelineAt));
                tk.name = item.name;
                all.push_back(tk);
            }
            setTakes(e, all, (int)all.size() - 1);
        }
        std::vector<Event> keep;
        for (Event& o : t->events) {
            if (o.end() <= at + 1e-9 || o.start >= at + len - 1e-9) {
                keep.push_back(o);
                continue;
            }
            if (o.start < at) {
                Event a = o;
                a.length = at - o.start;
                a.fadeOut = std::min(a.fadeOut, a.length);
                keep.push_back(a);
            }
            if (o.end() > at + len) {
                Event b = o;
                const double cut = at + len - o.start;
                b.id = p.newId();
                b.start = at + len;
                b.length = o.end() - b.start;
                b.offset += cut * o.rate;
                b.fadeIn = std::min(b.fadeIn, b.length);
                for (Envelope& env : b.envelopes)
                    for (EnvPoint& pt : env.points) pt.time -= cut;
                keep.push_back(b);
            }
        }
        t->events = keep;
        t->events.push_back(e);
        t->sortEvents();
        made.push_back(e.id);
    }
    return made;
}

}  // namespace reno
