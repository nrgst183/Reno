// The compute shaders the program carries: SPIR-V made at development time (tools/build_shaders.sh runs tools/gpukernels.py) and kept in the source as arrays, so no shader compiler is needed to build.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace reno {
namespace gpu {

struct ShaderBlob {
    const char* name;
    const uint32_t* words;
    size_t count;
};
const ShaderBlob* findShaderBlob(const std::string& name);
const std::vector<const ShaderBlob*>& allShaderBlobs();
// The workgroup size a module declares (OpExecutionMode LocalSize); false when it has none.
bool shaderLocalSize(const ShaderBlob& b, uint32_t& x, uint32_t& y, uint32_t& z);

}  // namespace gpu
}  // namespace reno
