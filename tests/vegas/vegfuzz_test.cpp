// vegfuzz_test: the sample VEGAS projects, damaged in many ways (bytes changed, chunk sizes made wrong, cut short), must
// be read without crashing, hanging or tripping the sanitizers; at worst they fail to open. Build under ASan + UBSan.
//   vegfuzz_test [samples folder] [--rounds N] [--seed S]
#include "../../src/vegas/vegas.h"
#include "../../src/vegas/vegformat.h"
#include "../../src/core/base.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <string>
#include <vector>
#include "../testdata.h"

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) ++gPass;
    else ++gFail;
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : ": ", detail.c_str());
    std::fflush(stdout);
}

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint64_t next()
    {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    size_t below(size_t n) { return n ? (size_t)(next() % n) : 0; }
};

// One damaged copy: a few kinds of damage, as files get damaged or as another writer would differ.
static std::string damage(const std::string& in, Rng& r, std::string& how)
{
    std::string d = in;
    if (d.size() < 64) return d;
    switch (r.below(6)) {
        case 0: {                                       // a few bytes changed anywhere
            const int n = 1 + (int)r.below(8);
            for (int k = 0; k < n; ++k) d[r.below(d.size())] = (char)r.next();
            how = strfmt("%d bytes changed", n);
            break;
        }
        case 1: {                                       // a 64-bit size (after a GUID) made huge, zero or a little wrong
            const size_t at = 16 + r.below(d.size() - 24);
            uint64_t v;
            std::memcpy(&v, &d[at], 8);
            const int kind = (int)r.below(4);
            v = kind == 0 ? ~(uint64_t)0 - r.below(16) : kind == 1 ? 0 : kind == 2 ? v + 1 + r.below(64) : v - 1 - r.below(64);
            std::memcpy(&d[at], &v, 8);
            how = strfmt("8 bytes at %zu set to %llu", at, (unsigned long long)v);
            break;
        }
        case 2: {                                       // a 32-bit count or length made huge or negative
            const size_t at = r.below(d.size() - 4);
            const uint32_t v = r.below(2) ? 0xFFFFFFFFu - (uint32_t)r.below(16) : 0x7FFFFFFFu;
            std::memcpy(&d[at], &v, 4);
            how = strfmt("4 bytes at %zu set to %u", at, v);
            break;
        }
        case 3: {                                       // cut short
            const size_t n = r.below(d.size());
            d.resize(n);
            how = strfmt("cut to %zu bytes", n);
            break;
        }
        case 4: {                                       // a stretch zeroed
            const size_t at = r.below(d.size()), n = std::min(d.size() - at, 1 + r.below(256));
            std::memset(&d[at], 0, n);
            how = strfmt("%zu bytes zeroed at %zu", n, at);
            break;
        }
        default: {                                      // a stretch repeated (a chunk written twice)
            const size_t at = r.below(d.size()), n = std::min(d.size() - at, 1 + r.below(512));
            d.insert(at, d.substr(at, n));
            how = strfmt("%zu bytes at %zu repeated", n, at);
            break;
        }
    }
    return d;
}

int main(int argc, char** argv)
{
    std::string dir = testDataDir("RENO_SAMPLES");
    int rounds = 60;
    uint64_t seed = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--rounds" && i + 1 < argc) rounds = std::atoi(argv[++i]);
        else if (a == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else dir = a;
    }
    std::vector<std::string> files;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        if (it->path().extension() == ".veg") files.push_back(it->path().string());
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::printf("SKIP: no .veg files in %s\nSUMMARY: 0 passed, 0 failed\n", dir.c_str());
        return 0;
    }
    Rng r(seed);
    int opened = 0, refused = 0, slowest = 0;
    std::string slowestWhat;
    for (const std::string& f : files) {
        std::string raw;
        if (!readFile(f, raw)) continue;
        for (int k = 0; k < rounds; ++k) {
            std::string how;
            const std::string bad = damage(raw, r, how);
            const std::string what = pathFileName(f) + ": " + how;
            // (on a thread of its own: a read that never ends is a failure, not a test that never ends)
            const auto t0 = std::chrono::steady_clock::now();
            auto task = std::async(std::launch::async, [bad, f] {
                Project p;
                return importVegasData(bad, f, p).ok;
            });
            if (task.wait_for(std::chrono::seconds(60)) != std::future_status::ready) {
                check(false, "a damaged project is read in reasonable time", what);
                std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
                std::fflush(stdout);
                std::_Exit(1);
            }
            const int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            if (ms > slowest) {
                slowest = ms;
                slowestWhat = what;
            }
            if (task.get()) ++opened;
            else ++refused;
        }
    }
    std::printf("%d damaged copies of %d projects: %d opened, %d refused; slowest %d ms (%s)\n", opened + refused, (int)files.size(), opened, refused,
                slowest, slowestWhat.c_str());
    check(true, strfmt("%d damaged copies read without a crash or a hang", opened + refused));
    check(slowest < 20000, "no damaged copy takes over 20 s to read", strfmt("%d ms: %s", slowest, slowestWhat.c_str()));
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
