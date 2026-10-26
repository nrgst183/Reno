// Tests of the Vulkan compute path (src/ofx/vk*.cpp): the loader, the device, buffers, the 8-bit and float frame
// conversions (including the rounding of the CPU path's store()), a kernel with push constants, and chains (a frame kept
// on the card for the next run), and failure handling. Skips with a message when there is no Vulkan driver (set
// VK_ICD_FILENAMES to SwiftShader's json and RENO_VK_ALLOW_SOFTWARE=1 to run it on a CPU implementation).
#include "../../src/ofx/vkgpu.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using namespace reno;

static int gFailures = 0, gChecks = 0;
#define CHECK(cond, ...) do { ++gChecks; if (!(cond)) { ++gFailures; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static int roundNonNegative(float v) { const int i = (int)v; return i + ((v - (float)i) >= 0.5f ? 1 : 0); }
static float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

// What the CPU path's store() writes for a premultiplied float pixel (8-bit frames), in memory order.
static void cpuPack8(const float* p, uint8_t* o, bool bgra)
{
    float a3 = p[3];
    if (!(a3 >= 0.f)) a3 = 0.f;
    const float a = clamp01(a3);
    auto q = [&](float c) { c = std::isfinite(c) ? std::min(std::max(c, 0.f), a) : 0.f; return (uint8_t)roundNonNegative(c * 255.f); };
    o[bgra ? 2 : 0] = q(p[0]); o[1] = q(p[1]); o[bgra ? 0 : 2] = q(p[2]); o[3] = (uint8_t)roundNonNegative(a * 255.f);
}
static void cpuUnpack8(const uint8_t* s, float* d, bool bgra)
{
    const float k = 1.f / 255.f;
    d[0] = s[bgra ? 2 : 0] * k; d[1] = s[1] * k; d[2] = s[bgra ? 0 : 2] * k; d[3] = s[3] * k;
}

int main()
{
    std::string why;
    const std::vector<gpu::DeviceInfo> devs = gpu::listDevices(&why);
    if (devs.empty()) {
        std::printf("vk_test: SKIPPED - no Vulkan device (%s)\n", why.c_str());
        // (the loader and the failure path still work)
        gpu::setEnabled(true);
        CHECK(!gpu::ready(&why), "ready() must be false with no device");
        gpu::Plan plan;
        plan.begin(4, 4);
        plan.add("copy_px", 0, 1, 4, 4);
        std::vector<uint8_t> in(64, 7), out(64, 0);
        gpu::FrameIO io;
        io.w = io.h = 4; io.in = in.data(); io.out = out.data(); io.inRowBytes = io.outRowBytes = 16;
        CHECK(!gpu::run(plan, io), "run() must fail quietly with no device");
        CHECK(out[0] == 0, "nothing written");
        std::printf("vk_test: %d checks, %d failures (status: %s)\n", gChecks, gFailures, gpu::status().c_str());
        return gFailures ? 1 : 0;
    }
    std::printf("devices:\n");
    for (const auto& d : devs)
        std::printf("  [%d] %s  %s  Vulkan %s  %llu MB  %s%s\n", d.index, d.name.c_str(), d.typeName().c_str(), d.apiVersionText().c_str(),
                    (unsigned long long)(d.localMemoryBytes >> 20), d.usable ? "usable" : "not usable: ", d.note.c_str());

    // a switched-off GPU does nothing
    {
        gpu::Plan plan;
        plan.begin(4, 4);
        plan.add("copy_px", 0, 1, 4, 4);
        std::vector<uint8_t> in(64, 7), out(64, 0);
        gpu::FrameIO io;
        io.w = io.h = 4; io.in = in.data(); io.out = out.data(); io.inRowBytes = io.outRowBytes = 16;
        CHECK(!gpu::run(plan, io), "run() with the GPU off must say false");
    }
    gpu::setEnabled(true);
    if (!gpu::ready(&why)) {
        std::printf("vk_test: SKIPPED - %s\n", why.c_str());
        return 0;
    }
    std::printf("using: %s\n", gpu::status().c_str());
    CHECK(gpu::hasShader("copy_px") && gpu::hasShader("pack_u8"), "shaders are compiled in");

    std::mt19937 rng(12345);
    const int sizes[][2] = {{257, 131}, {16, 16}, {1, 1}, {640, 360}, {31, 7}};
    for (bool bgra : {true, false}) {
        for (const auto& sz : sizes) {
            const uint32_t w = (uint32_t)sz[0], h = (uint32_t)sz[1];
            // ---- 8-bit: copy
            std::vector<uint8_t> in((size_t)w * h * 4), out((size_t)w * h * 4, 0xAB);
            for (auto& v : in) v = (uint8_t)(rng() & 255);
            gpu::Plan plan;
            plan.begin(w, h, "copy");
            plan.add("copy_px", 0, 1, w, h);
            gpu::FrameIO io;
            io.w = w; io.h = h; io.bgra = bgra; io.in = in.data(); io.out = out.data(); io.inRowBytes = io.outRowBytes = (size_t)w * 4;
            gpu::RunStats st;
            CHECK(gpu::run(plan, io, &st), "copy run %ux%u", w, h);
            // expected: unpack then store(): colours above alpha are clamped to it
            int bad = 0;
            for (size_t i = 0; i < (size_t)w * h; ++i) {
                float f[4];
                cpuUnpack8(&in[i * 4], f, bgra);
                uint8_t e[4];
                cpuPack8(f, e, bgra);
                if (std::memcmp(e, &out[i * 4], 4)) ++bad;
            }
            CHECK(bad == 0, "8-bit copy %ux%u bgra=%d: %d pixels differ", w, h, (int)bgra, bad);
            // ---- 8-bit invert, with a row stride bigger than the row
            const size_t strideIn = (size_t)w * 4 + 12, strideOut = (size_t)w * 4 + 20;
            std::vector<uint8_t> in2(strideIn * h), out2(strideOut * h, 0);
            for (auto& v : in2) v = (uint8_t)(rng() & 255);
            gpu::Plan p2;
            p2.begin(w, h, "invert");
            const float k = 0.75f;
            gpu::Pass& pass = p2.add("invert", 0, 1, w, h);
            struct { uint32_t w, h; float k; uint32_t pad; } push = {w, h, k, 0};
            pass.setPush(push);
            gpu::FrameIO io2;
            io2.w = w; io2.h = h; io2.bgra = bgra; io2.in = in2.data(); io2.inRowBytes = strideIn; io2.out = out2.data(); io2.outRowBytes = strideOut;
            CHECK(gpu::run(p2, io2, &st), "invert run %ux%u", w, h);
            int worst = 0, wrong = 0;
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    float f[4];
                    cpuUnpack8(&in2[y * strideIn + (size_t)x * 4], f, bgra);
                    float g[4];
                    if (f[3] > 0.f) {
                        const bool has = f[3] > 1e-6f;
                        for (int c = 0; c < 3; ++c) {
                            const float rs = has ? f[c] / f[3] : 0.f;
                            g[c] = (rs + (1.f - 2.f * rs) * k) * f[3];
                        }
                        g[3] = f[3];
                    } else g[0] = g[1] = g[2] = g[3] = 0.f;
                    uint8_t e[4];
                    cpuPack8(g, e, bgra);
                    const uint8_t* o = &out2[y * strideOut + (size_t)x * 4];
                    for (int c = 0; c < 4; ++c) { const int d = std::abs((int)e[c] - (int)o[c]); if (d > worst) worst = d; if (d > 1) ++wrong; }
                }
            }
            CHECK(wrong == 0 && worst <= 1, "8-bit invert %ux%u: worst %d, %d channels off by more than 1", w, h, worst, wrong);
            // the padding bytes of the output rows are left alone
            if (strideOut > (size_t)w * 4) CHECK(out2[(size_t)w * 4] == 0, "output padding untouched");
        }
        // ---- float: copy (rows flipped twice = same), with NaN / Inf / negative alpha
        {
            const uint32_t w = 67, h = 29;
            std::vector<float> in((size_t)w * h * 4), out((size_t)w * h * 4, -5.f);
            std::uniform_real_distribution<float> d(-0.5f, 1.5f);
            for (auto& v : in) v = d(rng);
            in[10] = NAN; in[11] = INFINITY; in[12] = -INFINITY; in[3] = -0.25f; in[7] = NAN;
            gpu::Plan plan;
            plan.begin(w, h, "copy");
            plan.add("copy_px", 0, 1, w, h);
            gpu::FrameIO io;
            io.w = w; io.h = h; io.isFloat = true; io.in = (const uint8_t*)in.data(); io.out = (uint8_t*)out.data(); io.inRowBytes = io.outRowBytes = (size_t)w * 16;
            CHECK(gpu::run(plan, io), "float copy run");
            int bad = 0;
            for (size_t i = 0; i < (size_t)w * h; ++i) {
                float p[4] = {in[i * 4], in[i * 4 + 1], in[i * 4 + 2], in[i * 4 + 3]};
                if (!(p[3] >= 0.f)) p[3] = 0.f;
                for (int c = 0; c < 4; ++c) {
                    const float e = std::isfinite(p[c]) ? p[c] : 0.f;
                    if (e != out[i * 4 + c]) ++bad;
                }
            }
            CHECK(bad == 0, "float copy: %d values differ", bad);
        }
    }

    // ---- chains: the second run takes the first run's result from the card
    {
        const uint32_t w = 200, h = 100;
        std::vector<uint8_t> a((size_t)w * h * 4), b(a.size()), c(a.size()), ref1(a.size()), ref2(a.size());
        for (auto& v : a) v = (uint8_t)(rng() & 255);
        gpu::Plan plan;
        plan.begin(w, h, "invert");
        gpu::Pass& pass = plan.add("invert", 0, 1, w, h);
        struct { uint32_t w, h; float k; uint32_t pad; } push = {w, h, 1.0f, 0};
        pass.setPush(push);
        gpu::FrameIO io1;
        io1.w = w; io1.h = h; io1.in = a.data(); io1.out = b.data(); io1.inRowBytes = io1.outRowBytes = (size_t)w * 4; io1.outSerial = 777;
        gpu::RunStats s1, s2, s3;
        CHECK(gpu::run(plan, io1, &s1) && !s1.inputStayed, "first run");
        gpu::FrameIO io2 = io1;
        io2.in = b.data(); io2.out = c.data(); io2.inSerial = 777; io2.outSerial = 778;
        CHECK(gpu::run(plan, io2, &s2) && s2.inputStayed, "second run takes the frame from the card");
        // the same without the shortcut
        std::vector<uint8_t> c2(a.size());
        gpu::FrameIO io3 = io2;
        io3.out = c2.data(); io3.inSerial = 0; io3.outSerial = 0;
        CHECK(gpu::run(plan, io3, &s3) && !s3.inputStayed, "third run uploads");
        CHECK(c == c2, "a chained run gives the same picture as an uploaded one");
        // a serial that does not match is not trusted
        gpu::FrameIO io4 = io2;
        io4.in = b.data(); io4.inSerial = 12345; io4.out = c2.data();
        CHECK(gpu::run(plan, io4, &s3) && !s3.inputStayed, "an unknown serial uploads");
    }

    // ---- bad input never crashes
    {
        gpu::Plan plan;
        plan.begin(8, 8);
        plan.add("no_such_shader", 0, 1, 8, 8);
        std::vector<uint8_t> in(256, 1), out(256, 0);
        gpu::FrameIO io;
        io.w = io.h = 8; io.in = in.data(); io.out = out.data(); io.inRowBytes = io.outRowBytes = 32;
        CHECK(!gpu::run(plan, io), "an unknown shader is refused");
        plan.passes.clear();
        CHECK(!gpu::run(plan, io), "a plan with no passes is refused");
        plan.begin(8, 8);
        plan.add("copy_px", 0, 5, 8, 8);
        CHECK(!gpu::run(plan, io), "a pass with a missing buffer is refused");
        plan.begin(8, 8);
        plan.add("copy_px", 0, 1, 8, 8);
        io.w = 9;
        CHECK(!gpu::run(plan, io), "a frame of another size than the plan's is refused");
        // a picture too big for the card's storage buffers
        gpu::Plan big;
        big.begin(60000, 60000);
        big.add("copy_px", 0, 1, 60000, 60000);
        gpu::FrameIO bio;
        bio.w = bio.h = 60000; bio.in = in.data(); bio.out = out.data(); bio.inRowBytes = bio.outRowBytes = 240000;
        CHECK(!gpu::run(big, bio), "an enormous frame is refused, not attempted");
    }
    // switching off and on again
    gpu::setEnabled(false);
    CHECK(!gpu::ready(), "off means off");
    gpu::setEnabled(true);
    CHECK(gpu::ready(), "and on again");
    gpu::shutdown();
    CHECK(gpu::ready(), "a device is made again after shutdown()");
    gpu::shutdown();

    std::printf("vk_test: %d checks, %d failures\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
