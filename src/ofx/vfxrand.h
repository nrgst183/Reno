// The random number generator VEGAS's older (DirectX) video effects use for their noise (Film Grain, TV Simulator...), as vfx1.dll builds it: a shift-register
// generator of 250 words (each new word is the one 250 steps back XOR the one 103 back), filled from the C library's rand() seeded with the effect's seed, with
// one bit set on a diagonal of 32 words so the register never starts all alike. rand() is the Microsoft one (a linear congruence), not this platform's, so the
// numbers are the same everywhere.
#pragma once
#include <cstdint>

namespace reno {
namespace ofx {
namespace builtin {

struct VegasShiftRandom {
    explicit VegasShiftRandom(uint32_t seed)
    {
        uint32_t st = seed;
        auto crand = [&st]() { st = st * 214013u + 2531011u; return (st >> 16) & 0x7fffu; };
        for (int k = 249; k >= 0; --k) {
            uint32_t a = crand() * 4u;
            a += crand() << 17;
            a += crand();
            w_[k] = a;
        }
        crandState_ = st;
        uint32_t m = 0xffffffffu, b = 0x80000000u;
        for (int k = 0; k < 32; ++k, m >>= 1, b >>= 1) w_[3 + 7 * k] = (w_[3 + 7 * k] & m) | b;
    }
    // where the C library's rand() stands after the 750 numbers the register was filled with (VEGAS's other noise tables go on from there)
    uint32_t randState() const { return crandState_; }
    // the next 32-bit word
    uint32_t next()
    {
        const int a = i0_, b = i1_;
        w_[a] ^= w_[b];
        i0_ = a == 249 ? 0 : a + 1;
        i1_ = b == 249 ? 0 : b + 1;
        return w_[a];
    }
private:
    uint32_t w_[250];
    int i0_ = 0, i1_ = 0x67;
    uint32_t crandState_ = 0;
};

}
}
}
