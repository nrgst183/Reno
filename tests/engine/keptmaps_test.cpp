// The maps the GPU plans of Scene Rotation, 360 Stabilization, Dual Fish Eye Stitching and Bezier Masking make from the
// settings alone (keptMap): they used to stay in function statics until the program ended (a 4K Scene Rotation map is
// 132 MB, a stitch map up to 240 MB). Now a map is kept while an instance that used it lives, under a total limit, and the
// same settings still give the same map.
//   keptmaps_test
#include "../../src/ofx/vkfilters.h"
#include <cstdio>
#include <cstring>

using namespace reno::ofx::builtin;

static int gFail = 0;
static void check(bool ok, const char* name)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++gFail;
}

static bool sceneMap(const void* owner, double yaw, int W, int H, std::vector<float>& map)
{
    const Def* d = findDef("com.magixsoftware:panorotation");
    if (!d) return false;
    GpuCheckCase cs("yaw", {{"mx.ofx.plugin.rotate.y.angle", {yaw}}});
    const GpuTestSettings set(*d, cs);
    Ctx c;
    set.apply(c);
    c.owner = owner;
    SceneSetup su;
    if (!sceneRotationSetupGpu(c, W, H, su)) return false;
    map = su.map;
    return true;
}

int main()
{
    const int A = 1, B = 2, C = 3;
    const int W = 640, H = 320;
    const size_t one = (size_t)W * H * 4 * sizeof(float);
    std::vector<float> m1, m2, m3;
    check(keptMapBytes() == 0, "nothing kept at first");
    check(sceneMap(&A, 40, W, H, m1) && m1.size() == (size_t)W * H * 4, "Scene Rotation: a map");
    check(keptMapBytes() == one, "it is kept");
    check(sceneMap(&B, 40, W, H, m2) && m2 == m1 && keptMapBytes() == one, "a second instance with the same settings shares it");
    dropKeptMaps(&A);
    check(keptMapBytes() == one, "one of them gone: still kept for the other");
    check(sceneMap(&B, 70, W, H, m3) && m3 != m1 && keptMapBytes() == one, "its settings changed: the new map replaces the old one");
    check(sceneMap(&B, 40, W, H, m2) && m2 == m1, "back to the first settings: the same map as before");
    dropKeptMaps(&B);
    check(keptMapBytes() == 0, "the last instance gone: nothing is kept");

    // the limit: maps of many instances (each with its own settings) never hold more than 384 MB together
    {
        size_t most = 0;
        bool same = true;
        for (int i = 0; i < 40; ++i) {
            std::vector<float> m;
            const int big = 1280 * 2, bigH = 1280;      // 52 MB each
            if (!sceneMap((const void*)(intptr_t)(100 + i), 10 + i, big, bigH, m)) { same = false; break; }
            most = std::max(most, keptMapBytes());
        }
        check(same && most <= ((size_t)384 << 20) && most > ((size_t)200 << 20), "many instances: the kept maps stay under the limit");
        for (int i = 0; i < 40; ++i) dropKeptMaps((const void*)(intptr_t)(100 + i));
        check(keptMapBytes() == 0, "and are all given back when the instances go");
    }
    // the generic store: a map is made once per key, while kept
    {
        int made = 0;
        auto mk = [&](std::vector<float>& v) { ++made; v.assign(1000, 1.f); };
        KeptMap a = keptMap(&C, "test", "k1", mk);
        KeptMap b = keptMap(&C, "test", "k1", mk);
        check(made == 1 && a == b && a->size() == 1000, "same kind and key: made once");
        KeptMap c2 = keptMap(&C, "other", "k1", mk);
        check(made == 2 && c2 != a, "another kind: a map of its own");
        dropKeptMaps(&C);
        check(keptMapBytes() == 0 && a && a->size() == 1000, "dropped: the store lets go, a holder keeps its copy");
    }
    std::printf("SUMMARY: %s\n", gFail ? "failed" : "passed");
    return gFail ? 1 : 0;
}
