// Smart rendering's plan: which frames of a render are a source file's own frames, to be copied as they are (see
// docs/NOTES.md, "Smart rendering", for exactly when that is).
#pragma once
#include "project.h"
#include "../media/smartrender.h"
#include <functional>
#include <memory>

namespace reno {

struct SmartCopy {
    int64_t firstFrame = 0;                     // the render's frame
    int64_t frames = 0;
    std::shared_ptr<const SmartSource> source;
    int firstGop = 0, gopCount = 0;             // the groups of the source's SmartSource::gops that fill those frames
};

struct SmartPlan {
    std::vector<SmartCopy> copies;              // in order, not overlapping
    std::string why;                            // when nothing is copied: what stands in the way (first reason found)
    int64_t copiedFrames() const
    {
        int64_t n = 0;
        for (const SmartCopy& c : copies) n += c.frames;
        return n;
    }
    // The copy that holds frame k, or null.
    const SmartCopy* at(int64_t k) const;
};

using SmartAnalyzer = std::function<bool(const std::string& path, std::shared_ptr<const SmartSource>* out, std::string* why)>;

// 'p' is the project as it is rendered; the render is 'count' frames of fpsNum/fpsDen from time 'from', picture outW x outH
// (rotated: the output rotation is in use). Frames are copied only where the whole picture is one event's source frame, untouched
// (smartplan.cpp lists what "untouched" is); and only whole closed groups of pictures of the source.
SmartPlan planSmartRender(const Project& p, double from, int64_t count, int fpsNum, int fpsDen, int outW, int outH, bool rotated,
                          bool transparent, const SmartAnalyzer& analyze = analyzeSmartSource);

}  // namespace reno
