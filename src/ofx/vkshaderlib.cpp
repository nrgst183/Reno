#include "vkshaderlib.h"
#include <map>

// The generated arrays: one header per shader (vkshaders/<name>.spv.h defines static const uint32_t spv_<name>[]), the list of headers (vkshaders/all_arrays.inc) and the table
// (vkshaders/all_table.inc: {"name", spv_name, N}, lines), all written by tools/build_shaders.sh.
namespace reno {
namespace gpu {

namespace {
#include "vkshaders/all_arrays.inc"
const ShaderBlob kBlobs[] = {
#include "vkshaders/all_table.inc"
    {nullptr, nullptr, 0}
};
}  // namespace

const std::vector<const ShaderBlob*>& allShaderBlobs()
{
    static const std::vector<const ShaderBlob*> all = [] {
        std::vector<const ShaderBlob*> v;
        for (const ShaderBlob* b = kBlobs; b->name; ++b) v.push_back(b);
        return v;
    }();
    return all;
}

const ShaderBlob* findShaderBlob(const std::string& name)
{
    static const std::map<std::string, const ShaderBlob*> byName = [] {
        std::map<std::string, const ShaderBlob*> m;
        for (const ShaderBlob* b : allShaderBlobs()) m[b->name] = b;
        return m;
    }();
    auto it = byName.find(name);
    return it == byName.end() ? nullptr : it->second;
}

bool shaderLocalSize(const ShaderBlob& b, uint32_t& x, uint32_t& y, uint32_t& z)
{
    if (!b.words || b.count < 5 || b.words[0] != 0x07230203u) return false;
    size_t i = 5;
    while (i < b.count) {
        const uint32_t w = b.words[i];
        const uint32_t op = w & 0xffffu, n = w >> 16;
        if (n == 0 || i + n > b.count) return false;
        if (op == 16 && n >= 6 && b.words[i + 2] == 17) {
            x = b.words[i + 3];
            y = b.words[i + 4];
            z = b.words[i + 5];
            return true;
        }
        i += n;
    }
    return false;
}

}  // namespace gpu
}  // namespace reno
