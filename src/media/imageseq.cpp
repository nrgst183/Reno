// Image sequences (see media.h): the numbering of the frames' names.
#include "media.h"
#include <cstdlib>

namespace reno {

namespace {
const char* const kPrefix = "imgseq:";

// The name split round its last run of digits: "dir/shot_" "0012" ".png".
bool splitNumber(const std::string& path, std::string& head, std::string& digits, std::string& tail)
{
    const size_t slash = path.find_last_of("/\\");
    const size_t nameStart = slash == std::string::npos ? 0 : slash + 1;
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot < nameStart) dot = path.size();
    size_t end = dot;
    size_t begin = end;
    while (begin > nameStart && path[begin - 1] >= '0' && path[begin - 1] <= '9') --begin;
    if (begin == end || end - begin > 9) return false;
    head = path.substr(0, begin);
    digits = path.substr(begin, end - begin);
    tail = path.substr(end);
    return true;
}

std::string numbered(const std::string& head, size_t width, long n, const std::string& tail)
{
    std::string d = std::to_string(n);
    if (d.size() < width) d = std::string(width - d.size(), '0') + d;
    return head + d + tail;
}
}  // namespace

bool imageSequencePattern(const std::string& firstFile, std::string& pattern, int& firstNumber)
{
    std::string head, digits, tail;
    if (!splitNumber(firstFile, head, digits, tail)) return false;
    firstNumber = (int)std::strtol(digits.c_str(), nullptr, 10);
    // '%' in the rest of the name would be read as a pattern: doubled
    auto esc = [](const std::string& s) {
        std::string o;
        for (char c : s) { if (c == '%') o += "%%"; else o += c; }
        return o;
    };
    // a name padded with zeros (0001) has a fixed width; one that is not (1, 2 ... 10) has none
    const bool padded = digits.size() > 1 && digits[0] == '0';
    pattern = esc(head) + (padded ? strfmt("%%0%dd", (int)digits.size()) : std::string("%d")) + esc(tail);
    return true;
}

int imageSequenceLength(const std::string& firstFile)
{
    std::string head, digits, tail;
    if (!splitNumber(firstFile, head, digits, tail) || !fileExists(firstFile)) return 0;
    const long first = std::strtol(digits.c_str(), nullptr, 10);
    const bool padded = digits.size() > 1 && digits[0] == '0';
    const size_t width = padded ? digits.size() : 0;
    int n = 1;
    while (n < 1000000 && fileExists(numbered(head, width, first + n, tail))) ++n;
    return n;
}

std::string imageSequenceSpec(const std::string& firstFile, double fps) { return strfmt("%s%.6f:", kPrefix, fps) + firstFile; }

bool parseImageSequenceSpec(const std::string& spec, std::string& firstFile, double& fps)
{
    if (spec.compare(0, 7, kPrefix) != 0) return false;
    const size_t colon = spec.find(':', 7);
    if (colon == std::string::npos) return false;
    fps = std::strtod(spec.substr(7, colon - 7).c_str(), nullptr);
    firstFile = spec.substr(colon + 1);
    return fps > 0 && fps <= 1000 && !firstFile.empty();
}

}  // namespace reno
