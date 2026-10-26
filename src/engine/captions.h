// VEGAS's closed captions (manual: "Closed Captioning"). A caption is a command marker whose command is 608CC1 ..
// 608CC4 (the CEA-608 caption channels CC1 .. CC4) and whose parameter is the caption in VEGAS's caption markup:
//   {RCL}{ENM}{R14In00Wh}{TabOff1}First Row{R15In00Wh}{TabOff1}Second Row{EDM}{EOC}
// ({RCL} starts a pop-on caption, {RDC} a paint-on one, {RU2}/{RU3}/{RU4} a roll-up one; {Ryy..} places a row;
// {EDM} clears the screen, {ENM} the caption being loaded, {EOC} shows it; [BR] is a line break in plain text).
// Plain text typed as a caption becomes a pop-on caption (popOnMarkup).
//
// This file: the markup, what a channel shows at a moment (for the preview overlay), and reading/writing caption
// files (.scc, .srt, .smi, .rt, .txt, .mcc, .sub).
#pragma once
#include "project.h"
#include <string>
#include <vector>

namespace reno {

// One caption: when it shows (seconds), its channel (1..4: CC1..CC4), and its markup (or plain text).
struct Caption {
    double time = 0;
    int channel = 1;
    std::string markup;
};

// "608CC1" .. "608CC4" (any case) and the channel; false for other commands.
bool isCaptionCommand(const std::string& command, int* channel = nullptr);
std::string captionCommand(int channel);
// Whether text has caption markup in it ({...} codes).
bool hasCaptionMarkup(const std::string& s);
// Plain text (lines broken by [BR] or new lines) as a pop-on caption: up to four rows ending on row 15, each row cut
// to 32 characters (the CEA-608 row), centred by indent as far as the indents allow (multiples of 4) when 'centre'.
std::string popOnMarkup(const std::string& text, bool centre = false);
// The text a caption shows, the markup left out, rows joined by "[BR]" (or by 'lineBreak').
std::string captionText(const std::string& markupOrText, const std::string& lineBreak = "[BR]");
// A marker's caption markup: its parameter as it is when it has markup, plain text made pop-on.
std::string captionMarkupOf(const std::string& parameter);

// The project's captions of a channel (0: all), in time order.
std::vector<Caption> projectCaptions(const Project& p, int channel = 0);

// What a caption channel shows at a moment: the rows on the screen (1..15) with the column each starts at (0..31)
// and its text, its colour ("Wh", "Gr", "Bl", "Cy", "Rd", "Yl", "Ma") and whether it is italic or underlined.
struct CaptionRow {
    int row = 15, column = 0;
    std::string text;
    std::string color = "Wh";
    bool italic = false, underline = false;
};
std::vector<CaptionRow> captionScreenAt(const std::vector<Caption>& captions, double t);

// Reading a caption file (the kind from its extension). fps is the frame rate frame-based timecodes count in (SCC
// files are always 29.97). Captions without a channel are CC1.
bool readCaptionFile(const std::string& path, double fps, std::vector<Caption>& out, std::string* error = nullptr);
// The CEA-608 byte pairs of a Scenarist file's text, decoded (for tests and for .mcc).
bool parseScc(const std::string& text, std::vector<Caption>& out, std::string* error = nullptr);

// Writing them. The captions are taken in time order; a caption lasts until the next one of its channel (or 'end').
// The extra file a format links to is written beside it (.asx for .smi, .smil for QuickTime).
enum class CaptionFormat { Scc, Srt, Sub, Smi, QuickTime };
bool writeCaptionFile(const std::string& path, CaptionFormat format, const std::vector<Caption>& captions, double end, double fps,
                      std::string* error = nullptr, const std::string& mediaName = std::string());
// The text of an SCC file (CC1 and CC2 only, as SCC holds field 1), for tests.
std::string sccText(const std::vector<Caption>& captions, double offset = 0);

}  // namespace reno
