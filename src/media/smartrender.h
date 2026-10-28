// Smart rendering's description of a source file's frames: which stretches of a video stream can be copied packet for packet
// into a render (see docs/NOTES.md, "Smart rendering"). Plain C++: the FFmpeg work is in smartrender.cpp and ffmpeg_writer.cpp.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace reno {

// A closed group of pictures: starts with a key packet that needs nothing before it, ends before the next closed key packet,
// and shows a run of consecutive frames of its own that no other group shows.
struct SmartGop {
    int64_t startFrame = 0;     // the first frame it shows (0: the stream's first frame; frames are counted at the stream's rate)
    int frames = 0;             // how many it shows (= the packets it holds)
    int64_t keyDts = 0, keyPts = 0;     // its first packet (in the stream's time base), to find it again
    int keySize = 0;
    int64_t minPts = 0;         // the time of the first frame it shows
    int delay = 0;              // the most any picture is shown after the one its position in the packets gives (frames)
};

struct SmartSource {
    std::string path;
    int streamIndex = -1;
    int codecId = 0;                    // FFmpeg's AVCodecID
    std::string codecName;
    int width = 0, height = 0;
    int pixFmt = -1;                    // FFmpeg's AVPixelFormat
    int profile = -99, level = -99;
    int fieldOrder = 0;                 // FFmpeg's AVFieldOrder
    int colorRange = 0, colorSpace = 0, colorPrimaries = 0, colorTrc = 0;
    int sarNum = 1, sarDen = 1;
    int tbNum = 1, tbDen = 1000;        // the stream's time base
    int fpsNum = 0, fpsDen = 1;         // its constant frame rate
    std::vector<uint8_t> extradata;     // the codec's own header (H.264/HEVC parameter sets), as the container keeps it
    int64_t frames = 0;                 // frames in the stream
    bool rotated = false;               // the stream asks players to turn it
    std::vector<SmartGop> gops;         // the groups that can be copied, in order
};

// Reads the packets of 'path's video stream (not the pictures) and describes them. False with *why when the stream cannot be
// copied from at all (a codec without closed groups, frames not on a constant rate...). Results are kept for the next call
// while the file is unchanged.
bool analyzeSmartSource(const std::string& path, std::shared_ptr<const SmartSource>* out, std::string* why);

// A codec header (a stream's extradata) as the list of its parameter sets, whichever way it is kept (H.264/HEVC: the
// configuration record of MP4 or Annex B start codes; the SPS, PPS and VPS only, not the encoder's own notes in SEI). Other codecs: the bytes.
std::vector<std::string> smartHeaderSets(int codecId, const uint8_t* data, int size);

// An H.264/HEVC packet of a source whose header is a configuration record (MP4, Matroska: every NAL unit has its length in front)
// in the form encoders give it and muxers expect (start codes). False if 'in' is not made that way; true and an empty *out (use the
// packet as it is) when the source already has start codes or the codec is another.
bool smartPacketToAnnexB(int codecId, const std::vector<uint8_t>& extradata, const uint8_t* in, int size, std::vector<uint8_t>* out);

// Forgets what analyzeSmartSource kept.
void forgetSmartSources();

}  // namespace reno
