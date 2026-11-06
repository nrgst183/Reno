#include "vegfxlayouts.h"
#include <map>
#include <utility>

namespace reno {
namespace veg {

namespace {
struct Row {
    const char* effect;
    const char* setting;
    int whole;
};
const Row kRows[] = {
#include "vegfxlayouts.inc"
};
}  // namespace

int knownFxSettingIsWhole(const std::string& effectId, const std::string& setting)
{
    static const std::map<std::pair<std::string, std::string>, int> table = [] {
        std::map<std::pair<std::string, std::string>, int> t;
        for (const Row& r : kRows) t[{r.effect, r.setting}] = r.whole;
        return t;
    }();
    auto it = table.find({effectId, setting});
    return it == table.end() ? -1 : it->second;
}

}  // namespace veg
}  // namespace reno
