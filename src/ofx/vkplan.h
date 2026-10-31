// What an effect asks the GPU to do for one frame: a list of compute passes over a few buffers. No Vulkan in here, so the effects' code
// (src/ofx/vkeffects.cpp) and the tests can build plans without the API.
//
// Every frame buffer a plan sees holds premultiplied RGBA FLOATS, TOP ROW FIRST, like the CPU effects' Buf. The runner turns the host's frame (8-bit BGRA/RGBA
// or float RGBA, bottom row first) into buffer 0 before the first pass and buffer 1 into the host's frame after the last (rounding like the CPU path's store()),
// so a kernel works in float on the same formulas as the effect's row function.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace reno {
namespace gpu {

// A buffer: w x h elements of 'comps' floats (4: a picture, 1: a plane).
struct BufDesc {
    uint32_t w = 0, h = 0, comps = 4;
    size_t bytes() const { return (size_t)w * h * comps * sizeof(float); }
};

// One compute dispatch. The shader gets the buffers on bindings 0 (in0), 1 (in1), 2 (out), 3 (table) as named in the shader (std430 float / vec4 arrays) and
// push constants of 'pushSize' bytes (at most 128).
struct Pass {
    std::string shader;
    int in0 = 0;
    int in1 = -1;
    int out = 1;
    int table = -1;
    uint32_t threadsX = 0, threadsY = 1;
    uint8_t push[128] = {0};
    uint32_t pushSize = 0;
    template <class T> void setPush(const T& v)
    {
        static_assert(sizeof(T) <= 128, "push constants are at most 128 bytes");
        std::memcpy(push, &v, sizeof(T));
        pushSize = (uint32_t)sizeof(T);
    }
};

struct Plan {
    uint32_t w = 0, h = 0;              // the frame
    std::vector<BufDesc> bufs;
    std::vector<std::vector<float>> tables;
    // A table that starts with the frame as w * h * 4 floats (top row first) made on the card from an 8-bit frame of the host's (4x less to send than the floats):
    // tables[k] then holds only what follows those floats. bytes == nullptr for the other tables. The bytes must stay valid until run() returns.
    struct FrameTable { const uint8_t* bytes = nullptr; size_t rowBytes = 0; bool bgra = true; };
    std::vector<FrameTable> tableFrames;
    std::vector<Pass> passes;
    std::string name;                   // for the log
    Plan() {}
    void begin(uint32_t W, uint32_t H, const char* what = "")
    {
        w = W; h = H; name = what;
        bufs.assign(2, BufDesc{W, H, 4});
        tables.clear();
        tableFrames.clear();
        passes.clear();
    }
    int addBuf(uint32_t bw, uint32_t bh, uint32_t comps = 4)
    {
        bufs.push_back(BufDesc{bw, bh, comps});
        return (int)bufs.size() - 1;
    }
    int addTable(std::vector<float> t)
    {
        tables.push_back(std::move(t));
        tableFrames.emplace_back();
        return (int)tables.size() - 1;
    }
    // A table of the 8-bit frame 'bytes' (rows bottom first, 'rowBytes' apart) as floats, then 'tail'. -1 when the plan has such a table already.
    int addFrameTable(const uint8_t* bytes, size_t rowBytes, bool bgra, std::vector<float> tail = std::vector<float>())
    {
        for (const FrameTable& f : tableFrames) if (f.bytes) return -1;
        const int k = addTable(std::move(tail));
        tableFrames[(size_t)k].bytes = bytes;
        tableFrames[(size_t)k].rowBytes = rowBytes;
        tableFrames[(size_t)k].bgra = bgra;
        return k;
    }
    size_t frameFloats(size_t k) const { return tableFrames[k].bytes ? (size_t)w * h * 4 : 0; }
    size_t tableFloats(size_t k) const { return frameFloats(k) + tables[k].size(); }
    Pass& add(const char* shader, int in0, int out, uint32_t tx, uint32_t ty)
    {
        passes.emplace_back();
        Pass& p = passes.back();
        p.shader = shader;
        p.in0 = in0;
        p.out = out;
        p.threadsX = tx;
        p.threadsY = ty;
        return p;
    }
};

}  // namespace gpu
}  // namespace reno
