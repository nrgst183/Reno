// vegmatrix_diff_test: tests the settings-matrix diff tool (vegmatrix_diff.cpp) on pairs of .veg files made up here.
//
// Without VMD_REAL: small project files are assembled byte by byte and changed in one known way (one word, a longer
// text, an added or removed chunk, a change in the third track); the tool must report exactly that. Then a folder with
// a manifest.txt: a base, a "control" (the base saved again, with one word of noise) and variants; noise must be hidden
// (shown with --noise), failed lines skipped, and the closing table must list the place and the files.
// With VMD_REAL (needs the whole engine library): a sample project is imported and written back by Reno's writer twice,
// once with a different render quality; the diff must find exactly the word the importer reads as render quality
// (video block 0 +0x1c of the project header).
//
//   vegmatrix_diff_test [sample.veg]        (the sample is used only with VMD_REAL)
#define VEGMATRIX_DIFF_NO_MAIN
#include "vegmatrix_diff.cpp"
#ifdef VMD_REAL
#include "../../src/vegas/vegas.h"
#endif
#include <functional>
#include <unistd.h>
#include "../testdata.h"

using namespace reno;

static int passed = 0, failed = 0;
static void check(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    (ok ? passed : failed)++;
}

// ------------------------------------------------------------------------------------------ assembling files
static std::string guidBytes(const char* g)
{
    // "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx": the first three groups little-endian, the rest in order
    std::string hex;
    for (const char* p = g; *p; ++p) if (*p != '-') hex += *p;
    std::string b;
    for (size_t i = 0; i < 16; ++i) b += (char)std::stoi(hex.substr(i * 2, 2), nullptr, 16);
    std::reverse(b.begin(), b.begin() + 4);
    std::reverse(b.begin() + 4, b.begin() + 6);
    std::reverse(b.begin() + 6, b.begin() + 8);
    return b;
}
static void put64(std::string& s, uint64_t v) { for (int i = 0; i < 8; ++i) s += (char)((v >> (8 * i)) & 0xff); }
static std::string pad8(std::string s) { while (s.size() % 8) s += '\0'; return s; }
static std::string leaf(const char* guid, const std::string& payload)
{
    std::string s = guidBytes(guid);
    put64(s, 24 + payload.size());
    return pad8(s + payload);
}
static std::string list(const char* type, const std::string& kids)
{
    std::string s = guidBytes(veg::G::List);
    put64(s, 40 + kids.size());
    return s + guidBytes(type) + kids;
}
static std::string vegFile(const std::string& kids)
{
    std::string s = guidBytes(veg::G::Riff);
    put64(s, 40 + kids.size());
    return s + guidBytes(veg::G::VegForm) + kids;
}
static std::string u16text(const std::string& t)
{
    std::string s;
    for (char c : t) { s += c; s += '\0'; }
    return s + std::string(2, '\0');
}
static void setWord(std::string& s, size_t off, uint32_t v) { for (int i = 0; i < 4; ++i) s[off + i] = (char)((v >> (8 * i)) & 0xff); }

struct Spec {
    uint32_t w1c = 3;               // header word at +0x1c
    uint32_t w40 = 7;               // header word at +0x40
    std::string text = "Hello";     // the summary text
    int tracks = 3;
    int changedTrack = -1;          // that track's header word at +8 is 99
};
static std::string build(const Spec& sp)
{
    std::string header(0x200, '\0');
    for (size_t i = 0; i < header.size(); i += 4) setWord(header, i, (uint32_t)(0x1000 + i));
    setWord(header, 0x1c, sp.w1c);
    setWord(header, 0x40, sp.w40);
    std::string tracks;
    for (int t = 0; t < sp.tracks; ++t) {
        std::string th(0x40, '\0');
        for (size_t i = 0; i < th.size(); i += 4) setWord(th, i, (uint32_t)(0x2000 + i));
        if (t == sp.changedTrack) setWord(th, 8, 99);
        tracks += list(veg::G::Track, leaf(veg::G::TrackHeader, th));
    }
    return vegFile(leaf(veg::G::Header, header) + list(veg::G::Tracks, tracks) + leaf(veg::G::SummaryData, u16text(sp.text)));
}

static std::unique_ptr<vmd::Loaded> make(const std::string& bytes, const std::string& name)
{
    std::unique_ptr<vmd::Loaded> l(new vmd::Loaded());
    l->name = name;
    l->buf.assign(bytes.begin(), bytes.end());
    veg::readChunkTree(veg::Bytes(l->buf.data(), l->buf.size()), l->tree);
    l->ok = true;
    return l;
}
static void putFile(const std::string& path, const std::string& data)
{
    std::ofstream f(path, std::ios::binary);
    f.write(data.data(), (std::streamsize)data.size());
}
static std::string capture(const std::function<void(FILE*)>& fn)
{
    FILE* f = std::tmpfile();
    fn(f);
    std::fflush(f);
    std::rewind(f);
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}
static bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

#ifndef VMD_REAL
int main()
{
    Spec base;
    const auto b = make(build(base), "base");
    check(b->tree.root.kids.size() == 3 && !b->tree.damaged, "the made-up project reads as 3 top chunks without damage");

    // identical
    check(vmd::diffFiles(*b, *make(build(base), "same")).empty(), "identical files: no changes");

    // one word
    {
        Spec s = base; s.w1c = 4;
        auto c = vmd::diffFiles(*b, *make(build(s), "w"));
        check(c.size() == 1 && c[0].kind == "word" && c[0].off == 0x1c && c[0].a == 3 && c[0].b == 4 && c[0].chunk == "/ProjectHeader",
              "one header word 3 -> 4 at +0x1c: reported once, with its place");
        check(!c.empty() && vmd::describe(c[0]).find("+0x1c: 00000003 -> 00000004") != std::string::npos, "its description names the offset and both values");
    }
    // a longer text
    {
        Spec s = base; s.text = "Hello world";
        auto c = vmd::diffFiles(*b, *make(build(s), "t"));
        check(c.size() == 1 && c[0].kind == "span" && c[0].chunk == "/SummaryData" && c[0].bText.size() && c[0].a != c[0].b,
              "a longer text: one span change with both sizes");
        check(!c.empty() && c[0].off == 10 && c[0].bText == " world", "the differing middle starts after 'Hello' and reads ' world'");
    }
    // text of the same size
    {
        Spec s = base; s.text = "Jello";
        auto c = vmd::diffFiles(*b, *make(build(s), "t2"));
        check(c.size() == 1 && c[0].kind == "word" && c[0].chunk == "/SummaryData", "a text changed in place: one word");
    }
    // added and removed chunk
    {
        Spec s = base; s.tracks = 4;
        auto c = vmd::diffFiles(*b, *make(build(s), "add"));
        check(c.size() == 1 && c[0].kind == "added" && c[0].chunk.find("[Tracks]/[Track]#") != std::string::npos, "a fourth track: one added chunk, the others not shifted");
        s.tracks = 2;
        c = vmd::diffFiles(*b, *make(build(s), "rem"));
        check(c.size() == 1 && c[0].kind == "removed", "a track fewer: one removed chunk");
    }
    // a change in the third track gets its sibling number
    {
        Spec s = base; s.changedTrack = 2;
        auto c = vmd::diffFiles(*b, *make(build(s), "t3"));
        check(c.size() == 1 && c[0].chunk == "/[Tracks]/[Track]#2/TrackHeader" && c[0].off == 8 && c[0].b == 99,
              "a word in the third track's header: path /[Tracks]/[Track]#2/TrackHeader +0x8");
        if (c.size()) std::printf("     %s\n", vmd::describe(c[0]).c_str());
    }
    // two changes in one file stay two
    {
        Spec s = base; s.w1c = 4; s.w40 = 8; s.text = "Hello!";
        check(vmd::diffFiles(*b, *make(build(s), "m")).size() == 3, "three separate changes are three entries");
    }

    // a folder
    char tmpl[] = "/tmp/vmdtestXXXXXX";
    const std::string dir = mkdtemp(tmpl);
    putFile(dir + "/00_base.veg", build(base));
    { Spec s = base; s.w40 = 8; putFile(dir + "/01_base_again_control.veg", build(s)); }
    { Spec s = base; s.w1c = 4; s.w40 = 9; putFile(dir + "/0003_Video.RenderQuality=Best.veg", build(s)); }
    { Spec s = base; s.w1c = 4; putFile(dir + "/0004_Video.RenderQuality=Best_again.veg", build(s)); }
    { Spec s = base; s.text = "Hello world"; putFile(dir + "/0005_Summary.Title.veg", build(s)); }
    putFile(dir + "/manifest.txt",
              "# Reno Settings Matrix\n"
              "00_base.veg\t\tok\t(base)\tthe base project\t\t\n"
              "01_base_again_control.veg\t00_base.veg\tok\t\topened and saved again\t\tcontrol\n"
              "0003_Video.RenderQuality=Best.veg\t00_base.veg\tok\tProject.Video\tProject.Video.RenderQuality = Best\tBest\t\n"
              "0004_Video.RenderQuality=Best_again.veg\t00_base.veg\tok\tProject.Video\tProject.Video.RenderQuality = Best\tBest\t\n"
              "0005_Summary.Title.veg\t00_base.veg\tok\tProject.Summary\tProject.Summary.Title = Hello world\tHello world\t\n"
              "(0006_Broken.veg)\t00_base.veg\tFAILED\tProject.Video\tProject.Video.Nothing = 1\t\tno property\n");
    {
        vmd::Options o;
        int n = 0;
        const std::string out = capture([&](FILE* f) { n = vmd::run(dir, {}, o, f); });
        check(n == 3, "folder: the three ok variants are compared (control and failed line are not)");
        check(has(out, "control 01_base_again_control.veg against 00_base.veg: 1 place(s)"), "folder: the control's one noisy place is found");
        check(has(out, "change: Project.Video.RenderQuality = Best"), "folder: the manifest's description is shown");
        check(has(out, "(1 noise change(s) hidden)"), "folder: the noise change in the first variant is hidden");
        check(!has(out, "+0x40: 00000007 -> 00000009"), "folder: the hidden noise word is not listed");
        check(has(out, "/ProjectHeader +0x1c: 00000003 -> 00000004"), "folder: the real change is listed");
        check(has(out, "==== places that change"), "folder: the closing table is there");
        check(has(out, "00000003 -> 00000004    0003_Video.RenderQuality=Best.veg, 0004_Video.RenderQuality=Best_again.veg"),
              "folder: the table lists both files under the one place and value change");
        check(!has(out, "0006_Broken"), "folder: the failed line is skipped");
        std::printf("----\n%s----\n", out.c_str());
    }
    {
        vmd::Options o; o.noise = true; o.noMap = true;
        const std::string out = capture([&](FILE* f) { vmd::run(dir, {}, o, f); });
        check(has(out, "(noise) /ProjectHeader +0x40: 00000007 -> 00000009"), "--noise shows the noise word, marked");
    }
    {
        vmd::Options o; o.brief = true;
        const std::string out = capture([&](FILE* f) { vmd::run(dir, {}, o, f); });
        check(has(out, "0005_Summary.Title.veg") && !has(out, "===="), "--brief: one line per file, no table");
    }
    {
        // loose files: first is the base
        vmd::Options o; o.noMap = true;
        const std::string out = capture([&](FILE* f) { vmd::run("", {dir + "/00_base.veg", dir + "/0005_Summary.Title.veg"}, o, f); });
        check(has(out, "/SummaryData size"), "loose files: base then variant");
    }
    {
        // a cut-off file does not crash the tool
        std::string full = build(base);
        putFile(dir + "/0007_cut.veg", full.substr(0, full.size() / 2));
        vmd::Options o; o.noMap = true; o.only = "0007";
        putFile(dir + "/manifest.txt", "00_base.veg\t\tok\t(base)\t\t\t\n0007_cut.veg\t00_base.veg\tok\t\tcut\t\t\n");
        const std::string out = capture([&](FILE* f) { vmd::run(dir, {}, o, f); });
        check(has(out, "0007_cut.veg"), "a cut-off file is handled (compared as far as it goes)");
    }
    std::string rm = "rm -rf " + dir;
    if (std::system(rm.c_str()) != 0) {}
    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
#else
int main(int argc, char** argv)
{
    // a sample project: the argument, else $RENO_SAMPLE_VEG, else the first .veg under $RENO_SAMPLES
    std::string sample = argc > 1 ? argv[1] : testSampleVeg();
    if (sample.empty()) {
        const std::vector<std::string> all = testVegFiles(testDataDir("RENO_SAMPLES"));
        sample = all.empty() ? testDataDir("RENO_SAMPLES") + "/(no .veg)" : all.front();
    }
    Project p;
    if (testDataMissing(sample)) return 0;
    if (!importVegasProject(sample, p).ok) { std::printf("FAIL cannot import %s\n", sample.c_str()); return 1; }
    Project q = p;
    q.settings.renderQuality = p.settings.renderQuality == "best" ? "preview" : "best";
    q.settings.deinterlace = p.settings.deinterlace == "blend" ? "none" : "blend";
    const std::string a = writeVegasData(p, "/tmp/vmd.veg", nullptr);
    const std::string b = writeVegasData(q, "/tmp/vmd_a_longer_name.veg", nullptr);
    check(!a.empty() && !b.empty(), "Reno's writer wrote both projects");
    auto la = make(a, "a"), lb = make(b, "b");
    auto c = vmd::diffFiles(*la, *lb);
    std::printf("changes: %zu\n", c.size());
    for (const auto& x : c) std::printf("   %s\n", vmd::describe(x).c_str());
    bool rq = false, di = false;
    for (const auto& x : c) {
        if (x.field.find("render quality") != std::string::npos) rq = true;
        if (x.field.find("deinterlace") != std::string::npos) di = true;
    }
    bool pathText = false, shifted = false;
    for (const auto& x : c) {
        if (x.kind == "text" && x.aText == "/tmp/vmd.veg") pathText = true;
        if ((x.field.find("render quality") != std::string::npos || x.field.find("deinterlace") != std::string::npos) && x.chunk.find("{after texts}") != std::string::npos) shifted = true;
    }
    check(rq && di && shifted && pathText, "longer path: render quality and deinterlace words still found, path reported as text");
    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
#endif
