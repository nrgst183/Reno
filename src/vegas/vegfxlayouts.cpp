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
std::function<int(const std::string&, const std::string&)>& source()
{
    static std::function<int(const std::string&, const std::string&)> fn;
    return fn;
}
}  // namespace

int knownFxSettingIsWhole(const std::string& effectId, const std::string& setting)
{
    static const std::map<std::pair<std::string, std::string>, int> table = [] {
        std::map<std::pair<std::string, std::string>, int> t;
        for (const Row& r : kRows) t[{r.effect, r.setting}] = r.whole;
        return t;
    }();
    auto it = table.find({effectId, setting});
    if (it != table.end()) return it->second;
    return source() ? source()(effectId, setting) : -1;
}

void setFxSettingKindSource(std::function<int(const std::string& effectId, const std::string& setting)> fn) { source() = std::move(fn); }

}  // namespace veg
}  // namespace reno
