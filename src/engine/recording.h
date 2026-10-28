// Recording sound: which input each armed track records from, how the sound of the inputs is shared out to the files,
// and what a finished recording does to the project (a new event, or a take of the selected events: VEGAS's
// punch-in and take recording). No sound card here: the program feeds the router, tests feed it made-up sound.
#pragma once
#include "project.h"
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace reno {

// One recording file's source and the tracks it goes onto (tracks with the same source share a file).
struct RecordSource {
    RecordInput input;
    std::vector<uint64_t> tracks;
    int channels() const { return input.inputBus ? 2 : input.channels; }
};
// The armed audio tracks, grouped by what they record from, in track order.
std::vector<RecordSource> planRecordSources(const Project& p, const std::set<uint64_t>& armed);

// Hands the sound of the sound card's inputs (interleaved, any number of channels per device) and of input busses to the
// files they belong to: a source of one channel gives that channel alone ('right' null), of two the channel and the next.
class RecordRouter {
public:
    // sink(source index, left, right (null for mono), frames)
    typedef std::function<void(int source, const float* left, const float* right, int frames)> Sink;
    void setup(const std::vector<RecordSource>& sources, const Sink& sink);
    // The devices to open ("" is the one chosen in Preferences) and how many channels each must give at least.
    std::vector<std::pair<std::string, int>> devices() const;
    // Sound from a device: 'frames' frames of 'channels' interleaved channels. A channel the device lacks is silent.
    void pushDevice(const std::string& device, const float* interleaved, int channels, int frames);
    // Sound of an input bus (after its insert effects).
    void pushInputBus(uint64_t id, const float* left, const float* right, int frames);

private:
    std::vector<RecordSource> sources_;
    Sink sink_;
    std::vector<float> a_, b_;
};

// Where a recording starts and where it stops by itself (stop <= start: it goes on until stopped). VEGAS records into
// the selected events of the armed tracks, over their length or the time selection (the punch-in with pre-roll and
// post-roll); with a time selection alone only inside it; else from the cursor on.
struct RecordSpan {
    double start = 0, stop = 0;
    bool intoEvents = false;        // some armed track has selected events to record into
};
RecordSpan planRecordSpan(const Project& p, const std::set<uint64_t>& armed, const std::set<uint64_t>& selectedEvents, double cursor, double selStart,
                          double selEnd);
// The selected events of a track that a recording on it goes into (in time order).
std::vector<uint64_t> recordTargetEvents(const Track& t, const std::set<uint64_t>& selectedEvents);

// A finished recording to put onto the timeline.
struct RecordCommit {
    double start = 0;               // the timeline time at which recording began (file time 0, less 'lag')
    double lag = 0;                 // the input's delay: this much of the file's start is skipped
    bool loop = false;              // playback looped over [loopStart, loopEnd) while recording: each pass is a take
    double loopStart = 0, loopEnd = 0;
    struct Item {
        uint64_t track = 0;
        uint64_t mediaId = 0;
        double length = 0;          // of the file, seconds
        std::string name;
        std::vector<uint64_t> events;   // the selected events recorded into (none: a new event)
    };
    std::vector<Item> items;
};
// Makes the recorded events or takes; returns the ids of the events made or recorded into. Selected events get a new
// take of the file (one per loop pass), the last made the active one; with no selected event a new event is laid where the
// recording began (cutting back what it covers), or, when looping, over the loop region with a take per pass.
std::vector<uint64_t> commitRecording(Project& p, const RecordCommit& c);

}  // namespace reno
