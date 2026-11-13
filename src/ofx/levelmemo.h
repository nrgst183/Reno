// Answers of a per-pixel function of an 8-bit picture, kept by the pixel's levels (builtins).
#pragma once
#include "vfxcolour.h"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace reno {
namespace ofx {
namespace builtin {

// VEGAS's OpenCL kernels read an 8-bit picture as levels (vcol::readLevels), so a pixel's answer is fixed by its four
// levels; a frame of video has far fewer distinct colours than pixels (flat areas, a few hundred thousand colours in
// two million pixels), and the costly kernels (Color Corrector (Secondary), the Chroma Keyer's key) were doing the
// same arithmetic over and over. Each thread keeps the last answers it worked out for one render of one effect, by
// the levels, in a table of its own; 'stamp' (one per render) tells this render's answers from older ones.
class LevelMemo {
public:
    static uint32_t newStamp()
    {
        static std::atomic<uint32_t> next{0};
        uint32_t s = next.fetch_add(1, std::memory_order_relaxed) + 1;
        if (s == 0) s = next.fetch_add(1, std::memory_order_relaxed) + 1;     // (0 marks an empty slot)
        return s;
    }
    // The pixel's four levels as one key, as vcol::readLevel finds them; false if a value is outside the levels' range
    // (the caller works that pixel out directly).
    static bool keyOf(const float* p, uint32_t& key)
    {
        uint32_t k = 0;
        for (int c = 0; c < 4; ++c) {
            const float t = p[c] * 255.0f + 0.5f;
            if (!(t >= 0.0f && t < 256.0f)) return false;
            k |= (uint32_t)(int)t << (8 * c);
        }
        key = k;
        return true;
    }
    // The pixel read as levels (exactly what vcol::readLevels gives for it).
    static void levelsOf(uint32_t key, float* in)
    {
        for (int c = 0; c < 4; ++c) in[c] = vcol::kByteLevels.level[(key >> (8 * c)) & 255];
    }
    // This thread's table.
    static LevelMemo& mine()
    {
        thread_local LevelMemo m;
        return m;
    }
    // The answer kept for 'key' in render 'stamp', or null.
    const float* find(uint32_t key, uint32_t stamp) const
    {
        const Slot& s = slots_[index(key)];
        return (s.stamp == stamp && s.key == key) ? s.v : nullptr;
    }
    void put(uint32_t key, uint32_t stamp, const float* v)
    {
        Slot& s = slots_[index(key)];
        s.key = key;
        s.stamp = stamp;
        std::memcpy(s.v, v, sizeof s.v);
    }

private:
    static constexpr int kBits = 14;            // 16384 answers (384 KB a thread)
    struct Slot {
        uint32_t key = 0, stamp = 0;
        float v[4] = {0, 0, 0, 0};
    };
    static size_t index(uint32_t key) { return (size_t)((key * 2654435761u) >> (32 - kBits)); }
    std::vector<Slot> slots_ = std::vector<Slot>((size_t)1 << kBits);
};

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
