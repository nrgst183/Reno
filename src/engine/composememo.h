// The video engine's memory of finished warps and blends, kept for the next preview picture (engine_video.cpp).
#pragma once

namespace reno {

// Forgets the finished warps and blends kept for the next preview picture, and the frames they were made from (a
// project closed: they belong to it).
void clearComposeMemo();

}  // namespace reno
