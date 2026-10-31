// reno_cli fxbench: how long each built-in effect takes per frame, on the CPU and on the card, and what a chain of effects costs.
//
//   reno_cli fxbench [--cpu | --gpu | --both] [--size WxH] [--repeat N] [--only TEXT] [--chain a,b,c] [--device N] [--software] [--float]
//
// For every built-in effect (default settings, a test picture, 8-bit BGRA unless --float): the CPU time (the effect's own code, threads and all), and for those
// with Vulkan kernels the GPU time of the kernels alone (timestamps), and the GPU time as the host sees it - frame up, kernels, frame down. Then a chain
// of effects (default: Brightness and Contrast, HSL Adjust, Gaussian Blur, Sharpen) four ways: on the CPU; on the card one by one (each result comes
// back); on the card with the frame kept there between effects but still downloaded; and on the card with the downloads left out (what Reno does for a
// chain). The two card chains must give the same bytes. Times are the median of --repeat runs (default 5) after one to warm up, in milliseconds per frame.
#include "vkeffects.h"
#include "builtins.h"
#include "gpupick.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

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

bool parseSize(const std::string& t, int* w, int* h)
{
    int a = 0, b = 0;
    char x = 0;
    if (std::sscanf(t.c_str(), "%d%c%d", &a, &x, &b) != 3 || (x != 'x' && x != 'X') || a < 16 || b < 16 || a > 16384 || b > 16384) return false;
    *w = a;
    *h = b;
    return true;
}

std::vector<std::string> splitList(const std::string& s)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

}  // namespace

int runFxBench(const std::vector<std::string>& args)
{
    int W = 1920, H = 1080, repeat = 5, device = -1;
    bool doCpu = true, doGpu = true, isFloat = false, software = false;
    std::string only, chainText = "brightnessandcontrast,hsladjust,gaussianblur,sharpen";
    for (size_t k = 0; k < args.size(); ++k) {
        const std::string& a = args[k];
        auto need = [&](std::string* v) {
            if (k + 1 >= args.size()) { std::fprintf(stderr, "reno_cli fxbench: %s needs a value\n", a.c_str()); return false; }
            *v = args[++k];
            return true;
        };
        std::string v;
        if (a == "--cpu") { doCpu = true; doGpu = false; }
        else if (a == "--gpu") { doCpu = false; doGpu = true; }
        else if (a == "--both") { doCpu = doGpu = true; }
        else if (a == "--float") isFloat = true;
        else if (a == "--software") software = true;
        else if (a == "--only") { if (!need(&only)) return 2; }
        else if (a == "--chain") { if (!need(&chainText)) return 2; }
        else if (a == "--size") {
            if (!need(&v)) return 2;
            if (!parseSize(v, &W, &H)) { std::fprintf(stderr, "reno_cli fxbench: --size wants WIDTHxHEIGHT (16 to 16384), e.g. 1920x1080\n"); return 2; }
        }
        else if (a == "--repeat") { if (!need(&v)) return 2; repeat = std::max(1, std::min(100, std::atoi(v.c_str()))); }
        else if (a == "--device") { if (!need(&v)) return 2; device = std::atoi(v.c_str()); }
        else {
            std::fprintf(stderr, "reno_cli fxbench: unknown option %s\nusage: reno_cli fxbench [--cpu | --gpu | --both] [--size WxH] [--repeat N] [--only TEXT] [--chain a,b,c] [--device N] [--software] [--float]\n", a.c_str());
            return 2;
        }
    }
    for (char& c : only) c = (char)std::tolower((unsigned char)c);
#ifdef _WIN32
    if (software) _putenv_s("RENO_VK_ALLOW_SOFTWARE", "1");
#else
    if (software) setenv("RENO_VK_ALLOW_SOFTWARE", "1", 1);
#endif
    bool gpuOk = false;
    if (doGpu) {
        if (device >= 0) gpu::selectDevice(device);
        gpu::setEnabled(true);
        std::string why;
        gpuOk = gpu::ready(&why);
        if (gpuOk) std::printf("GPU: %s\n", gpu::status().c_str());
        else std::printf("GPU: not usable (%s); only the CPU is timed\n", why.c_str());
    }
    std::printf("picture %dx%d %s, ms per frame, median of %d runs after a warm-up\n\n", W, H, isFloat ? "float" : "8-bit", repeat);
    const size_t stride = (size_t)W * (isFloat ? 16 : 4);
    std::vector<uint8_t> in(stride * (size_t)H), a(stride * (size_t)H), b(stride * (size_t)H);
    makeGpuTestPicture(W, H, isFloat, true, false, in.data(), stride);

    std::printf("%-40s %9s %11s %12s   %s\n", "effect", "CPU", "GPU kernel", "GPU+transfer", "faster");
    std::printf("%-40s %9s %11s %12s   %s\n", "", "", "", "", "  (GPU: plan on the CPU | fill staging | card copy up | kernels | card copy down | submit/wait overhead | read staging)");
    double cpuSum = 0, gpuSum = 0;
    int shown = 0;
    for (const BuiltinInfo& bi : builtinInfos()) {
        const Def* d = findDef(bi.id);
        if (!d || !d->render || d->temporal) continue;
        std::string low = bi.id;
        for (char& c : low) c = (char)std::tolower((unsigned char)c);
        if (!only.empty() && low.find(only) == std::string::npos) continue;
        const std::string shortId = bi.id.substr(bi.id.find(':') + 1);
        const GpuTestSettings set(*d, GpuCheckCase());
        double cpuMs = -1, kernMs = -1, gpuMs = -1;
        char parts[200] = "";
        if (doCpu) {
            Ctx c;
            set.apply(c);
            c.fps = 25;
            std::vector<double> t;
            bool ok = renderFrameCpu(*d, c, W, H, isFloat, true, in.data(), stride, a.data(), stride);       // (warm-up)
            for (int r = 0; r < repeat && ok; ++r) {
                const Clock::time_point t0 = Clock::now();
                ok = renderFrameCpu(*d, c, W, H, isFloat, true, in.data(), stride, a.data(), stride);
                t.push_back(msSince(t0));
            }
            if (ok) cpuMs = median(t);
        }
        if (gpuOk && d->gpu) {
            Ctx c;
            set.apply(c);
            c.fps = 25;
            gpu::RunStats st;
            std::vector<double> t, k, pl, fi, up, dn, sy, rd;
            bool ok = renderFrameGpu(*d, c, W, H, isFloat, true, in.data(), stride, b.data(), stride, &st);
            for (int r = 0; r < repeat && ok; ++r) {
                const Clock::time_point t0 = Clock::now();
                ok = renderFrameGpu(*d, c, W, H, isFloat, true, in.data(), stride, b.data(), stride, &st);
                const double wall = msSince(t0);
                t.push_back(wall);
                k.push_back(st.gpuMs);
                pl.push_back(std::max(0.0, wall - st.totalMs));
                fi.push_back(st.fillMs);
                up.push_back(st.copyUpMs);
                dn.push_back(st.copyDownMs);
                sy.push_back(std::max(0.0, st.waitMs - st.copyUpMs - st.gpuMs - st.copyDownMs));
                rd.push_back(st.readMs);
            }
            if (ok) {
                gpuMs = median(t); kernMs = median(k);
                std::snprintf(parts, sizeof parts, "  [%.2f | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f]", median(pl), median(fi), median(up), kernMs, median(dn), median(sy), median(rd));
            }
        }
        char cs[16] = "-", ks[16] = "-", gs[16] = "-";
        if (cpuMs >= 0) std::snprintf(cs, sizeof cs, "%.2f", cpuMs);
        if (kernMs >= 0) std::snprintf(ks, sizeof ks, "%.2f", kernMs);
        if (gpuMs >= 0) std::snprintf(gs, sizeof gs, "%.2f", gpuMs);
        const char* faster = (cpuMs >= 0 && gpuMs >= 0) ? (gpuMs < cpuMs ? "GPU" : "CPU") : "";
        std::printf("%-40s %9s %11s %12s   %s%s\n", shortId.c_str(), cs, ks, gs, faster, parts);
        std::fflush(stdout);
        if (cpuMs >= 0) cpuSum += cpuMs;
        if (gpuMs >= 0) gpuSum += gpuMs;
        ++shown;
    }
    std::printf("\n%d effects. Sum of the lines: CPU %.0f ms, GPU+transfer %.0f ms\n", shown, cpuSum, gpuSum);

    // ---- a chain
    const std::vector<std::string> names = splitList(chainText);
    std::vector<const Def*> ds;
    for (const std::string& n : names) {
        const Def* d = findDef(n.find(':') == std::string::npos ? "com.vegascreativesoftware:" + n : n);
        if (!d || !d->render) { std::fprintf(stderr, "reno_cli fxbench: no built-in effect \"%s\"\n", n.c_str()); return 2; }
        ds.push_back(d);
    }
    if ((only.empty() || std::string("chain").find(only) != std::string::npos) && !ds.empty()) {
        std::vector<std::unique_ptr<GpuTestSettings>> sets;
        for (const Def* d : ds) sets.emplace_back(new GpuTestSettings(*d, GpuCheckCase()));
        const int n = (int)ds.size();
        std::printf("\nchain of %d effects (%s)\n", n, chainText.c_str());
        std::vector<uint8_t> buf[2] = {std::vector<uint8_t>(stride * (size_t)H), std::vector<uint8_t>(stride * (size_t)H)};
        double cpuT = -1, oneT = -1, keptT = -1, deferT = -1;
        if (doCpu) {
            std::vector<double> t;
            bool ok = true;
            for (int r = 0; r < repeat + 1 && ok; ++r) {
                const Clock::time_point t0 = Clock::now();
                const uint8_t* src = in.data();
                for (int i = 0; i < n && ok; ++i) {
                    Ctx c;
                    sets[(size_t)i]->apply(c);
                    c.fps = 25;
                    ok = renderFrameCpu(*ds[(size_t)i], c, W, H, isFloat, true, src, stride, buf[i % 2].data(), stride);
                    src = buf[i % 2].data();
                }
                if (r > 0) t.push_back(msSince(t0));
            }
            if (ok) cpuT = median(t);
        }
        bool allGpu = gpuOk;
        for (const Def* d : ds) if (!d->gpu) allGpu = false;
        if (allGpu) {
            std::vector<uint8_t> res[3];
            // mode 0: each effect on its own (the result comes back, the next goes up again); 1: the frame kept on the card but downloaded; 2: downloads left out
            for (int mode = 0; mode < 3; ++mode) {
                gpu::setChainDeferral(mode == 2);
                std::vector<double> t;
                bool ok = true;
                for (int r = 0; r < repeat + 1 && ok; ++r) {
                    gpu::dropResident();
                    const Clock::time_point t0 = Clock::now();
                    const uint8_t* src = in.data();
                    uint64_t prev = 0;
                    for (int i = 0; i < n && ok; ++i) {
                        Ctx c;
                        sets[(size_t)i]->apply(c);
                        c.fps = 25;
                        const uint64_t mine = mode == 0 ? 0 : 7000000000ull + (uint64_t)mode * 100000 + (uint64_t)r * 100 + (uint64_t)i + 1;
                        gpu::DeferOutput dflt(mode == 2 && i + 1 < n);
                        ok = renderFrameGpu(*ds[(size_t)i], c, W, H, isFloat, true, src, stride, buf[i % 2].data(), stride, nullptr, prev, mine);
                        prev = mine;
                        src = buf[i % 2].data();
                    }
                    if (r > 0) t.push_back(msSince(t0));
                }
                if (!ok) { std::printf("  the chain failed on the card (%s)\n", lastGpuFailure().c_str()); break; }
                res[mode].assign(buf[(n - 1) % 2].begin(), buf[(n - 1) % 2].end());
                (mode == 0 ? oneT : mode == 1 ? keptT : deferT) = median(t);
            }
            gpu::setChainDeferral(true);
            gpu::dropResident();
            const bool same = !res[0].empty() && res[0] == res[1] && res[1] == res[2];
            std::printf("  card chains give %s bytes\n", same ? "the same" : "DIFFERENT");
        } else if (doGpu) {
            std::printf("  (not all of these effects have Vulkan kernels, or there is no usable card: the card chains are left out)\n");
        }
        auto show = [](const char* what, double ms) { if (ms >= 0) std::printf("  %-52s %9.2f ms\n", what, ms); };
        show("CPU, one effect after the other", cpuT);
        show("GPU, one by one (up and down for each effect)", oneT);
        show("GPU, frame kept on the card, downloaded each time", keptT);
        show("GPU, frame kept on the card, one upload and one download", deferT);
    }
    gpu::shutdown();
    return 0;
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
