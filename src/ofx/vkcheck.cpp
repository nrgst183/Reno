// reno_cli vkcheck: the Vulkan devices of this computer, and for every built-in effect with GPU kernels the same test picture through the CPU code and the GPU at several settings: the
// biggest and mean difference and the time of each. The report goes to the screen and to vkcheck_report.txt (in the folder the command runs in).
//
//   reno_cli vkcheck [--only TEXT] [--size WxH] [--repeat N] [--device N] [--software] [--report FILE]
//
// 8-bit pictures should agree within 1 level per channel and float ones within 1e-4; a line that does not is marked DIFFERS.
#include "vkeffects.h"
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <stdlib.h>
#endif

namespace reno {
namespace ofx {
namespace builtin {
namespace {

typedef std::chrono::steady_clock Clock;
double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

double median(std::vector<double> v)
{
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

struct Report {
    std::string text;
    void line(const char* fmt, ...)
    {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        text += buf;
        text += "\n";
        std::fputs(buf, stdout);
        std::fputs("\n", stdout);
        std::fflush(stdout);
    }
};

struct Diff { double maxd = 0, mean = 0; size_t over = 0, n = 0; };

// 8-bit: in levels; float: in values. 'over' counts the values beyond the tolerance (1 level, 1e-4 + 1e-5 * |v|).
Diff compare(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, size_t values, bool isFloat, double floatTol = 1e-4)
{
    Diff d;
    d.n = values;
    double sum = 0;
    for (size_t i = 0; i < values; ++i) {
        double x, y, tol;
        if (isFloat) {
            float fa, fb;
            std::memcpy(&fa, a.data() + i * 4, 4);
            std::memcpy(&fb, b.data() + i * 4, 4);
            x = fa; y = fb;
            tol = floatTol + 1e-5 * std::fabs(x);
        } else {
            x = a[i]; y = b[i];
            tol = 1.0;
        }
        const double dd = std::fabs(x - y);
        sum += dd;
        if (dd > d.maxd) d.maxd = dd;
        if (dd > tol) ++d.over;
    }
    d.mean = values ? sum / (double)values : 0;
    return d;
}

bool parseSize(const std::string& t, int* w, int* h)
{
    int a = 0, b = 0;
    char x = 0;
    if (std::sscanf(t.c_str(), "%d%c%d", &a, &x, &b) != 3 || (x != 'x' && x != 'X') || a < 16 || b < 16 || a > 16384 || b > 16384) return false;
    *w = a;
    *h = b;
    return true;
}

void putEnv(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

}  // namespace

int runVkCheck(const std::vector<std::string>& args)
{
    std::string only, reportFile = "vkcheck_report.txt";
    int W = 1280, H = 720, repeat = 5, device = -1;
    bool software = false;
    for (size_t k = 0; k < args.size(); ++k) {
        const std::string& a = args[k];
        auto need = [&](std::string* v) {
            if (k + 1 >= args.size()) { std::fprintf(stderr, "reno_cli vkcheck: %s needs a value\n", a.c_str()); return false; }
            *v = args[++k];
            return true;
        };
        std::string v;
        if (a == "--only") { if (!need(&only)) return 2; }
        else if (a == "--size") {
            if (!need(&v)) return 2;
            if (!parseSize(v, &W, &H)) { std::fprintf(stderr, "reno_cli vkcheck: --size wants WIDTHxHEIGHT (16 to 16384), e.g. 1920x1080\n"); return 2; }
        }
        else if (a == "--repeat") { if (!need(&v)) return 2; repeat = std::max(1, std::min(100, std::atoi(v.c_str()))); }
        else if (a == "--device") { if (!need(&v)) return 2; device = std::atoi(v.c_str()); }
        else if (a == "--report") { if (!need(&reportFile)) return 2; }
        else if (a == "--software") software = true;
        else {
            std::fprintf(stderr, "reno_cli vkcheck: unknown option %s\n"
                                 "usage: reno_cli vkcheck [--only TEXT] [--size WxH] [--repeat N] [--device N] [--software] [--report FILE]\n", a.c_str());
            return 2;
        }
    }
    if (software) putEnv("RENO_VK_ALLOW_SOFTWARE", "1");
    for (char& c : only) c = (char)std::tolower((unsigned char)c);

    Report rep;
    rep.line("reno_cli vkcheck - the built-in effects on the CPU and on Vulkan");
    rep.line("picture %dx%d, GPU time = the median of %d runs after one to warm up", W, H, repeat);
    rep.line("");
    std::string why;
    const std::vector<gpu::DeviceInfo> devices = gpu::listDevices(&why);
    if (devices.empty()) rep.line("Vulkan devices: none (%s)", why.c_str());
    else {
        rep.line("Vulkan devices:");
        for (const gpu::DeviceInfo& d : devices)
            rep.line("  [%d] %s  (%s, Vulkan %s, %.1f GB, %s%s%s)", d.index, d.name.c_str(), d.typeName().c_str(), d.apiVersionText().c_str(),
                     (double)d.localMemoryBytes / (1024.0 * 1024.0 * 1024.0), d.usable ? "usable" : "NOT usable", d.software ? ", software" : "",
                     d.usable || d.note.empty() ? "" : (", " + d.note).c_str());
        for (const gpu::DeviceInfo& d : devices) {
            if (!d.fcKnown) { rep.line("      float controls of [%d]: not asked (the device is below Vulkan 1.2)", d.index); continue; }
            static const char* const indep[] = {"32-bit only", "all", "none"};
            rep.line("      float controls of [%d]: roundingModeRTE32 %d, roundingModeRTZ32 %d, denormPreserve32 %d, denormFlushToZero32 %d, signedZeroInfNanPreserve32 %d, denormBehaviorIndependence %s, roundingModeIndependence %s",
                     d.index, d.fcRteF32 ? 1 : 0, d.fcRtzF32 ? 1 : 0, d.fcDenormPreserveF32 ? 1 : 0, d.fcDenormFlushF32 ? 1 : 0, d.fcSignedZeroInfNanF32 ? 1 : 0,
                     d.fcDenormIndependence >= 0 && d.fcDenormIndependence < 3 ? indep[d.fcDenormIndependence] : "?", d.fcRoundingIndependence >= 0 && d.fcRoundingIndependence < 3 ? indep[d.fcRoundingIndependence] : "?");
        }
    }
    if (device >= 0) gpu::selectDevice(device);
    gpu::setEnabled(true);
    if (!gpu::ready(&why)) {
        rep.line("");
        rep.line("No Vulkan device can be used: %s", why.c_str());
        rep.line("(Built-in effects render on the CPU. A software Vulkan implementation is only used with --software.)");
        FILE* f = std::fopen(reportFile.c_str(), "wb");
        if (f) { std::fwrite(rep.text.data(), 1, rep.text.size(), f); std::fclose(f); }
        return 3;
    }
    rep.line("");
    rep.line("using %s", gpu::status().c_str());
    {
        const gpu::DeviceInfo cur = gpu::currentDevice();
        rep.line("float controls in every kernel (RoundingModeRTE and DenormPreserve, 32-bit): %s", cur.floatControlsUsed ? "ON" : (cur.fcKnown ? "off (the driver lacks RTE or denorm preserve for 32-bit floats, or RENO_VK_NO_FLOAT_CONTROLS is set)" : "off (below Vulkan 1.2: the driver's default rounding and denormal handling apply)"));
    }
    rep.line("");
    rep.line("%-34s %-10s %-11s %8s %9s %7s %9s %9s %9s  %s", "effect / settings", "format", "", "max diff", "mean diff", "over", "CPU ms", "GPU ms", "kernel ms", "");

    int total = 0, bad = 0;
    double cpuSum = 0, gpuSum = 0;
    const int modes[2] = {0, 2};
    for (const std::string& id : gpuEffectIds()) {
        const Def* d = findDef(id);
        if (!d || !d->gpu) continue;
        std::string low = id;
        for (char& c : low) c = (char)std::tolower((unsigned char)c);
        std::string shortId = id.substr(id.find(':') + 1);
        if (!only.empty() && low.find(only) == std::string::npos) continue;
        for (const GpuCheckCase& cs : gpuCheckCases(*d)) {
            for (int mode : modes) {
                const bool isFloat = mode == 2, bgra = true;
                const size_t stride = (size_t)W * (isFloat ? 16 : 4);
                std::vector<uint8_t> in(stride * (size_t)H), a(stride * (size_t)H, 0), b(stride * (size_t)H, 0);
                makeGpuTestPicture(W, H, isFloat, bgra, true, in.data(), stride);
                const GpuTestSettings set(*d, cs);
                Ctx c1, c2;
                set.apply(c1);
                set.apply(c2);
                c1.fps = c2.fps = 25;
                char label[160];
                std::snprintf(label, sizeof label, "%s / %s", shortId.c_str(), cs.name.c_str());
                ++total;
                // the CPU: the first run, then the median of a few
                std::vector<double> cpuT;
                bool ok = true;
                for (int r = 0; r < std::min(3, repeat) && ok; ++r) {
                    const Clock::time_point t0 = Clock::now();
                    ok = renderFrameCpu(*d, c1, W, H, isFloat, bgra, in.data(), stride, a.data(), stride);
                    cpuT.push_back(msSince(t0));
                }
                if (!ok) { rep.line("%-34s %-10s CPU render failed", label, isFloat ? "float" : "8-bit"); ++bad; continue; }
                gpu::RunStats st;
                if (!renderFrameGpu(*d, c2, W, H, isFloat, bgra, in.data(), stride, b.data(), stride, &st)) {
                    rep.line("%-34s %-10s GPU render failed: %s", label, isFloat ? "float" : "8-bit", gpu::status().c_str());
                    ++bad;
                    continue;
                }
                std::vector<double> gpuT, kernT;
                for (int r = 0; r < repeat; ++r) {
                    const Clock::time_point t0 = Clock::now();
                    gpu::RunStats s2;
                    if (!renderFrameGpu(*d, c2, W, H, isFloat, bgra, in.data(), stride, b.data(), stride, &s2)) break;
                    gpuT.push_back(msSince(t0));
                    kernT.push_back(s2.gpuMs);
                }
                const Diff df = compare(a, b, (size_t)W * (size_t)H * 4, isFloat, gpuFloatTolerance(*d));
                const double cpuMs = median(cpuT), gpuMs = median(gpuT), kMs = median(kernT);
                const bool fine = df.over == 0;
                if (!fine) ++bad;
                cpuSum += cpuMs;
                gpuSum += gpuMs;
                rep.line("%-34s %-10s %-11s %8.*f %9.*f %7zu %9.2f %9.2f %9.2f  %s", label, isFloat ? "float" : "8-bit", "", isFloat ? 6 : 0, df.maxd, isFloat ? 7 : 4, df.mean,
                         df.over, cpuMs, gpuMs, kMs, fine ? "ok" : "DIFFERS");
            }
        }
    }

    if (only.empty() || std::string("chain").find(only) != std::string::npos) {
        const char* const names[3] = {"brightnessandcontrast", "colorbalance", "sharpen"};
        const Def* ds[3];
        bool have = true;
        for (int i = 0; i < 3; ++i) {
            ds[i] = findDef(std::string("com.vegascreativesoftware:") + names[i]);
            if (!ds[i] || !ds[i]->gpu) have = false;
        }
        if (have) {
            const size_t stride = (size_t)W * 4;
            std::vector<uint8_t> in(stride * (size_t)H), cpuA(stride * (size_t)H), cpuB(stride * (size_t)H), gA(stride * (size_t)H), gB(stride * (size_t)H);
            makeGpuTestPicture(W, H, false, true, false, in.data(), stride);
            GpuCheckCase cs[3] = {GpuCheckCase("chain", {{"Brightness", {0.1}}, {"Contrast", {0.2}}}), GpuCheckCase("chain", {{"Red", {0.2}}, {"Blue", {-0.1}}}),
                                  GpuCheckCase("chain", {{"Amount", {0.5}}})};
            GpuTestSettings* sets[3];
            for (int i = 0; i < 3; ++i) sets[i] = new GpuTestSettings(*ds[i], cs[i]);
            std::vector<double> cpuT, gpuT;
            bool ok = true;
            for (int r = 0; r < std::min(3, repeat) && ok; ++r) {
                const Clock::time_point t0 = Clock::now();
                const uint8_t* src = in.data();
                std::vector<uint8_t>* dst = &cpuA;
                for (int i = 0; i < 3 && ok; ++i) {
                    Ctx c;
                    sets[i]->apply(c);
                    c.fps = 25;
                    ok = renderFrameCpu(*ds[i], c, W, H, false, true, src, stride, dst->data(), stride);
                    src = dst->data();
                    dst = dst == &cpuA ? &cpuB : &cpuA;
                }
                cpuT.push_back(msSince(t0));
            }
            const uint8_t* cpuResult = (3 % 2 == 1) ? cpuA.data() : cpuB.data();
            std::vector<uint8_t> cpuFinal(cpuResult, cpuResult + stride * (size_t)H);
            for (int r = 0; r < repeat + 1 && ok; ++r) {
                const Clock::time_point t0 = Clock::now();
                const uint8_t* src = in.data();
                std::vector<uint8_t>* dst = &gA;
                uint64_t prev = 0;
                for (int i = 0; i < 3 && ok; ++i) {
                    Ctx c;
                    sets[i]->apply(c);
                    c.fps = 25;
                    const uint64_t mine = 9000000ull + (uint64_t)r * 10 + (uint64_t)i + 1;
                    ok = renderFrameGpu(*ds[i], c, W, H, false, true, src, stride, dst->data(), stride, nullptr, prev, mine);
                    prev = mine;
                    src = dst->data();
                    dst = dst == &gA ? &gB : &gA;
                }
                if (r > 0) gpuT.push_back(msSince(t0));
            }
            for (int i = 0; i < 3; ++i) delete sets[i];
            ++total;
            if (!ok) {
                rep.line("%-34s chain failed", "chain: bright, balance, sharpen");
                ++bad;
            } else {
                const Diff df = compare(cpuFinal, gA, (size_t)W * (size_t)H * 4, false);
                const bool fine = df.over == 0;
                if (!fine) ++bad;
                rep.line("%-34s %-10s %-11s %8.0f %9.4f %7zu %9.2f %9.2f %9s  %s", "chain: bright, balance, sharpen", "8-bit", "", df.maxd, df.mean, df.over, median(cpuT), median(gpuT), "",
                         fine ? "ok" : "DIFFERS");
            }
        }
    }

    rep.line("");
    rep.line("%d comparisons, %d with a problem. Summed over the lines: CPU %.0f ms, GPU %.0f ms (GPU time includes copying the picture to the card and back).", total, bad, cpuSum, gpuSum);
    rep.line("A line is DIFFERS when a value is more than 1 level (8-bit) or 1e-4 (float) away from the CPU's picture.");
    FILE* f = std::fopen(reportFile.c_str(), "wb");
    if (f) {
        std::fwrite(rep.text.data(), 1, rep.text.size(), f);
        std::fclose(f);
        std::printf("report written to %s\n", reportFile.c_str());
    } else {
        std::fprintf(stderr, "could not write %s\n", reportFile.c_str());
    }
    gpu::shutdown();
    return bad ? 1 : 0;
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
