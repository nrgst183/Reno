// Shared between the engine's files; not for the rest of Reno.
#pragma once
#include "engine.h"

namespace reno {

// How much of an event shows (or is heard) at time t because of the events it overlaps on its track:
// where two overlap, the earlier one fades out as the later one fades in.
double overlapWeight(const Track& tr, const Event& e, double t, bool crossfade = true);
// The same counting only 'others' (overlappingEvents: the events of e's track that can overlap it, in the track's
// order), so that many samples of one event do not each look through the whole track.
double overlapWeightAmong(const std::vector<const Event*>& others, const Event& e, double t, bool crossfade = true);
void overlappingEvents(const Track& tr, const Event& e, std::vector<const Event*>& out);
// Turns the compositor's shortcuts (warp's per-column path, the first layer copied onto the canvas, the finished
// frame not copied) off and on again, for tests that check they give the same pictures as the plain way.
void setComposeFastPaths(bool on);
// How often each shortcut was taken so far (warps in all, those on the per-column path, layers copied onto the canvas, frames handed on uncopied, warps and blends
// given from the preview's memory of the last ones).
void composeFastStats(uint64_t& axisWarps, uint64_t& warps, uint64_t& layerCopies, uint64_t& framesNotCopied, uint64_t& memoHits);
// dst += src * f, same depth (8-bit: clamped)
void addScaled(Image& dst, const Image& src, float f);

// The calling thread's nested-project counters (nesting depth, whether a final render is running, how many renders
// left a nested project's effects out).
struct NestedThreadState { int depth = 0, finalDepth = 0; uint64_t leftOut = 0; };
NestedThreadState nestedThreadState();
void setNestedThreadState(const NestedThreadState& st);

}  // namespace reno
