// VEGAS's Titles & Text animations ("_Bounce", "_Title02" ...), read from the TextGen assemblies of the user's own VEGAS install and evaluated the way its managed engine does
// (ScriptPortal.MediaSoftware.TextGen.CoreGraphics: Timeline / Row / ValueRow / Cascade / Block / Evaluator / EditableMatrix, read from the IL).
//
// The animations are data: 49 small XML documents (MetaTextCollectionItem) kept as resources inside ScriptPortal.MediaSoftware.TextGen.CoreGraphics.dll. They are NOT shipped
// with Reno; this file looks the DLL up at run time (RENO_VEGAS_DIR or the usual install folders), finds the document by its name and parses it. Without the DLL nothing here
// finds an animation and Reno's own simple motion is used.
//
// What an animation is. A tree of "rows" (the <automation> element) whose leaves are value rows: a property of the text ("MetaTextNodes.[0].Lines.[0].Words.[0].Characters.[0].EdTransform.Offset.Y")
// and a signal for it. The progress goes 0..1 over the event (the same for every row; the rows' own ranges are all 0..1). A signal is a block tree: a root block with a function
// (a curve of keyed control values, or an oscillator / parabola / noise), whose children (start, length, combine mode Join / Add / Multiply, scale) replace or add to it. A row can be
// marked as a cascade: its signal then runs for every following item of its list (every character of the line, every word ...) with a delay that grows along the list
// (the delay shape evaluated at the item's place in the list x the duration). A cascading row first copies its first item's saved state (transform, effects, opacity) to the others.
// What a property means: the offsets are canvas units (20 high), Y up; scale; shear; rotation in radians (clockwise); opacity; blur and glow amounts of the item's image effects.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace reno {
namespace ofx {
namespace vfxta {

// ------------------------------------------------------------------ a small XML reader (elements, attributes, text; entities)
struct XNode {
    std::string tag, text;
    std::vector<std::pair<std::string, std::string>> attr;
    std::vector<XNode> kids;
    const std::string* get(const char* a) const
    {
        for (const auto& p : attr)
            if (p.first == a) return &p.second;
        return nullptr;
    }
    std::string str(const char* a, const char* def = "") const { const std::string* s = get(a); return s ? *s : std::string(def); }
    double num(const char* a, double def) const { const std::string* s = get(a); return s && !s->empty() ? std::atof(s->c_str()) : def; }
    bool flag(const char* a, bool def) const { const std::string* s = get(a); return s ? (*s == "true" || *s == "1") : def; }
    const XNode* child(const char* t) const
    {
        for (const XNode& k : kids)
            if (k.tag == t) return &k;
        return nullptr;
    }
    std::string childText(const char* t, const char* def = "") const { const XNode* k = child(t); return k ? k->text : std::string(def); }
};

inline void xmlDecode(std::string& s)
{
    if (s.find('&') == std::string::npos) return;
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { o += s[i]; continue; }
        const size_t e = s.find(';', i);
        if (e == std::string::npos || e - i > 10) { o += s[i]; continue; }
        const std::string n = s.substr(i + 1, e - i - 1);
        if (n == "lt") o += '<';
        else if (n == "gt") o += '>';
        else if (n == "amp") o += '&';
        else if (n == "quot") o += '"';
        else if (n == "apos") o += '\'';
        else if (!n.empty() && n[0] == '#') {
            const long v = n.size() > 1 && (n[1] == 'x' || n[1] == 'X') ? std::strtol(n.c_str() + 2, nullptr, 16) : std::strtol(n.c_str() + 1, nullptr, 10);
            if (v > 0 && v < 128) o += (char)v;
            else o += '?';
        } else { o += s[i]; continue; }
        i = e;
    }
    s.swap(o);
}

// Parses one element (p at '<'); returns the position after it, or null on a malformed document.
inline const char* xmlElement(const char* p, const char* end, XNode& out, int depth = 0)
{
    if (depth > 64 || p >= end || *p != '<') return nullptr;
    ++p;
    const char* b = p;
    while (p < end && !std::isspace((unsigned char)*p) && *p != '>' && *p != '/') ++p;
    out.tag.assign(b, p);
    while (p < end) {
        while (p < end && std::isspace((unsigned char)*p)) ++p;
        if (p >= end) return nullptr;
        if (*p == '/') { if (p + 1 < end && p[1] == '>') return p + 2; return nullptr; }
        if (*p == '>') { ++p; break; }
        const char* an = p;
        while (p < end && *p != '=' && !std::isspace((unsigned char)*p) && *p != '>') ++p;
        std::string name(an, p);
        while (p < end && std::isspace((unsigned char)*p)) ++p;
        if (p >= end || *p != '=') return nullptr;
        ++p;
        while (p < end && std::isspace((unsigned char)*p)) ++p;
        if (p >= end || (*p != '"' && *p != '\'')) return nullptr;
        const char q = *p++;
        const char* vb = p;
        while (p < end && *p != q) ++p;
        if (p >= end) return nullptr;
        std::string v(vb, p);
        xmlDecode(v);
        out.attr.emplace_back(std::move(name), std::move(v));
        ++p;
    }
    while (p < end) {
        const char* tb = p;
        while (p < end && *p != '<') ++p;
        if (p > tb) { std::string t(tb, p); xmlDecode(t); out.text += t; }
        if (p >= end) return nullptr;
        if (p + 1 < end && p[1] == '/') {
            while (p < end && *p != '>') ++p;
            return p < end ? p + 1 : nullptr;
        }
        if (p + 3 < end && p[1] == '!' && p[2] == '-' && p[3] == '-') {
            const char* e = p + 4;
            while (e + 2 < end && !(e[0] == '-' && e[1] == '-' && e[2] == '>')) ++e;
            if (e + 2 >= end) return nullptr;
            p = e + 3;
            continue;
        }
        XNode k;
        p = xmlElement(p, end, k, depth + 1);
        if (!p) return nullptr;
        out.kids.push_back(std::move(k));
    }
    return nullptr;
}

// ------------------------------------------------------------------ .NET's System.Random (the noise evaluator seeds one)
struct NetRandom {
    int seedArray[56];
    int inext = 0, inextp = 21;
    explicit NetRandom(int seed)
    {
        const int MBIG = 2147483647;
        int subtraction = seed == (int)0x80000000 ? MBIG : (seed < 0 ? -seed : seed);
        int mj = 161803398 - subtraction;
        seedArray[55] = mj;
        int mk = 1;
        for (int i = 1; i < 55; ++i) {
            const int ii = (21 * i) % 55;
            seedArray[ii] = mk;
            mk = mj - mk;
            if (mk < 0) mk += MBIG;
            mj = seedArray[ii];
        }
        for (int k = 1; k < 5; ++k)
            for (int i = 1; i < 56; ++i) {
                seedArray[i] -= seedArray[1 + (i + 30) % 55];
                if (seedArray[i] < 0) seedArray[i] += MBIG;
            }
        inext = 0;
        inextp = 21;
    }
    int internalSample()
    {
        const int MBIG = 2147483647;
        int a = inext, b = inextp;
        if (++a >= 56) a = 1;
        if (++b >= 56) b = 1;
        int r = seedArray[a] - seedArray[b];
        if (r == MBIG) --r;
        if (r < 0) r += MBIG;
        seedArray[a] = r;
        inext = a;
        inextp = b;
        return r;
    }
    double nextDouble() { return internalSample() * (1.0 / 2147483647.0); }
    int next() { return internalSample(); }
};

// ------------------------------------------------------------------ evaluators (ParamLibrary)
struct ControlValue {
    double key = 0, value = 0;
    int type = 0;                // 0 Auto, 1 Adjustable, 2 SplitAdjustable, 3 Linear, 4 Fast, 5 Slow, 6 Smooth
    double in = 0, out = 0;
};

struct Fn {
    int kind = 0;                // 0 curve, 1 oscillator, 2 parabola, 3 noise
    double loops = 1, decay = 1, offset = 0, inner = 0, steps = 0;
    bool autoSub = false, continuous = false;
    std::vector<ControlValue> pts;
    double freq = 0, damp = 0, amp = 0;                      // oscillator
    double nfreq = 10, noct = 3;                             // noise
    std::vector<double> samples;
    std::vector<int> rnd;

    void seed(int s)
    {
        NetRandom r(s);
        samples.assign(1024, 0.0);
        rnd.assign(1024, 0);
        for (int i = 0; i < 1024; ++i) { rnd[i] = i; samples[i] = (r.nextDouble() - 0.5) * 2.0; }
        for (int i = 0; i < 1024; ++i) {
            const int t = rnd[i];
            const int j = r.next() % 1024;
            rnd[i] = rnd[j];
            rnd[j] = t;
        }
    }
    double noise1(double x) const
    {
        const int xi = (int)x;
        const int i0 = xi & 1023, i1 = (i0 + 1) & 1023;
        const double f0 = x - xi, f1 = f0 - 1.0;
        const double v0 = f0 * samples[rnd[i0]], v1 = f1 * samples[rnd[i1]];
        return v0 + f0 * f0 * (3.0 - 2.0 * f0) * (v1 - v0);
    }
    double perlin(double x) const
    {
        if (samples.size() != 1024) return 0;
        const int oct = (int)std::floor(noct + 0.5);
        double f = nfreq, a = 1.0, sum = 0;
        x *= f;
        for (int i = 0; i < oct; ++i) { sum += noise1(x) * a; x *= 2.0; a /= 2.0; }
        return sum;
    }
    // Evaluator.EvaluateInner for t in 0..1 (level: the block's scale)
    double inner01(double t, double level) const
    {
        switch (kind) {
        case 0: {
            const int n = (int)pts.size();
            if (n == 0) return 0.0;
            int lower = -1, upper = -1;
            double frac = 0;
            for (int i = 0; i < n; ++i) {
                const double k = pts[i].key;
                if (k <= t) continue;
                if (i == 0) { lower = 0; break; }
                const double prev = pts[i - 1].key;
                lower = prev == k ? i : i - 1;
                upper = i;
                frac = (t - prev) / (k - prev);
                break;
            }
            if (lower < 0) lower = n - 1;
            const ControlValue& a = pts[lower];
            if (upper < 0) return a.value;
            const ControlValue& b = pts[upper];
            if (a.type >= 3) {
                const double d = b.value - a.value;
                switch (a.type) {
                case 3: return a.value + d * frac;
                case 4: return a.value + d * (1.0 - (1.0 - frac) * (1.0 - frac));
                case 5: return a.value + d * frac * frac;
                default: {
                    const double s3 = frac * frac * 3.0, s2 = frac * frac * frac * 2.0;
                    return (s2 - s3 + 1.0) * a.value + (-s2 + s3) * b.value;
                }
                }
            }
            const double w = b.key - a.key;
            double vprev = a.value, wprev = w, vnext = b.value, wnext = w;
            if (lower > 0) { vprev = pts[lower - 1].value; wprev = a.key - pts[lower - 1].key; }
            if (upper + 1 < n) { vnext = pts[upper + 1].value; wnext = pts[upper + 1].key - b.key; }
            double m0, m1;
            if (a.type == 1 || a.type == 2) m0 = a.out;
            else { m0 = 0.5 * (a.value - vprev) + 0.5 * (b.value - a.value); m0 *= 2.0 * w / (wprev + w); }
            if (b.type == 1 || b.type == 2) m1 = -b.in;
            else { m1 = 0.5 * (b.value - a.value) + 0.5 * (vnext - b.value); m1 *= 2.0 * w / (wnext + w); }
            const double t2 = frac * frac, t3 = t2 * frac;
            return (2.0 * t3 - 3.0 * t2 + 1.0) * a.value + (-2.0 * t3 + 3.0 * t2) * b.value + (t3 - 2.0 * t2 + frac) * m0 + (t3 - t2) * m1;
        }
        case 1: {
            const double e = std::exp(-damp * 10.0 * t);
            return (amp * e * std::cos(6.283185307179586 * freq * t) - amp * e) * level;
        }
        case 2: {
            t = t < 0.5 ? 1.0 - t * 2.0 : (t - 0.5) * 2.0;
            return (1.0 - t * t) * level;
        }
        case 3: return perlin(t) * level;
        }
        return 0;
    }
    // Evaluator._Evaluate: loops, decay, offsets and steps, then the inner function
    double eval1(double t, double level) const
    {
        double scale = 1.0;
        if (loops > 1.0 || offset > 0.0 || inner > 0.0) {
            if (decay < 0.9999999) {
                double d = 1.0 / loops, start = 0.0, amplitude = 1.0;
                t = t + offset + inner * d;
                for (int i = 0; i < 32; ++i) {
                    if (t >= start && t < start + d) { t = (t - start) / d; break; }
                    start += d;
                    amplitude *= decay;
                    d *= decay;
                    if (d < 1e-6) { t = 1.0; break; }
                }
                scale = amplitude;
            } else {
                t = (t + offset) * loops + inner;
                if (!continuous) t = std::min(t, loops);
            }
            t = std::fmod(t, 1.0);
        }
        if (steps > 0.0) {
            const double s = 1.0 / steps;
            t = std::floor(t / s + 0.5) * s;
        }
        return inner01(t, level) * scale;
    }
    double eval(double t, double level) const
    {
        double v = eval1(t, level);
        if (autoSub) v -= eval1(0.0, level);
        return v;
    }
};

struct Block {
    bool root = false;
    double start = 0, length = 1, scale = 1;
    int combine = 0;             // 0 Join, 1 Add, 2 Multiply
    bool hasFn = false;
    Fn fn;
    std::vector<Block> kids;
};

inline double smoothLerp(double a, double b, double t)
{
    t = std::max(0.0, std::min(1.0, t));
    const double s = t * t * (3.0 - 2.0 * t);
    return a + (b - a) * s;
}

inline double blockEval(const Block& B, double t, double v, const Block* excl, bool* flag);

inline double blockValue(const Block& parent, const Block& b, double v, double t)
{
    (void)parent;
    bool f = false;
    if (t > b.start + b.length) return blockEval(b, 1.0, v, nullptr, &f);
    if (std::isinf(b.length)) return blockEval(b, t - b.start, v, nullptr, &f);
    return blockEval(b, (t - b.start) / b.length, v, nullptr, &f);
}

// Block.Evaluate (t within the block, v the value so far); 'excl': the child the outer call is evaluating around; 'flag' is set when a gap between two blocks is bridged
inline double blockEval(const Block& B, double t, double v, const Block* excl, bool* flag)
{
    if (B.hasFn && B.root && !excl) v = B.fn.eval(t, B.scale);
    const int n = (int)B.kids.size();
    double maxEnd = 0.0;
    const Block* firstAfter = nullptr;
    int containing = 0, before = 0;
    for (int i = 0; i < n; ++i) {
        const Block& c = B.kids[i];
        if (excl && &c == excl) break;
        if (c.start > t) { firstAfter = &c; break; }
        ++before;
        if (t >= c.start && t < c.length) ++containing;
        maxEnd = std::max(maxEnd, c.start + c.length);
    }
    double ctl = 0.0;
    if (!containing && before > 0 && t > maxEnd) {
        const double target = firstAfter ? firstAfter->start : (excl ? excl->start : 1.0);
        if (B.hasFn && B.fn.kind == 0) {
            for (const ControlValue& p : B.fn.pts)
                if (p.key >= maxEnd - 1e-9 && p.key <= target + 1e-9) { ctl = p.key; *flag = true; break; }
        }
    }
    double out = v;
    if (before > 0) {
        const Block* last = nullptr;
        for (int j = 0; j < before; ++j)
            if (t > B.kids[j].start) last = &B.kids[j];
        if (last) {
            bool f2 = false;
            const double pre = blockEval(B, last->start, v, last, &f2);
            out = blockValue(B, *last, pre, t);
            if (last->combine != 0 && B.root) {
                const double base = blockEval(B, t, v, last, &f2);
                out = last->combine == 1 ? base + out : (last->combine == 2 ? base * out : out);
            }
        }
    }
    if (*flag && ctl > maxEnd) {
        const double own = B.fn.eval(t, B.scale);
        if (t > ctl) {
            if (firstAfter && firstAfter->combine == 0 && firstAfter->start > ctl) {
                bool f3 = false;
                const double nx = blockEval(*firstAfter, 0.0, out, nullptr, &f3);
                out = smoothLerp(own, nx, (t - ctl) / (firstAfter->start - ctl));
            } else out = own;
        } else out = smoothLerp(own, out, (ctl - t) / (ctl - maxEnd));
    }
    if (!B.hasFn || B.root) return out;
    const double own = B.fn.eval(t, B.scale);
    if (before == 0) return own;
    return own * out;
}

// ------------------------------------------------------------------ rows
struct Row {
    std::string binding;
    bool cascadeable = false, cascadeOn = false;
    double cascadeDur = 0.2;
    std::string alias;
    bool hasShape = false;
    Fn shape;
    bool isValue = false;
    double initial = 0;
    bool hasSignal = false;
    Block signal;
    std::vector<Row> kids;
};

struct SavedNode {
    std::string kind;            // "Line", "Word", "Char"
    int index = 0;
    double mat[13];
    bool hasMat = false;
    int center = 9;
    bool fx[3] = {false, false, false};          // blur, glow, drop shadow enabled
    double fxAmount[3] = {0, 0, 0};
    double hblur = 0, vblur = 0;
    double opacity = 1;
};

struct Item {
    std::string name;
    double mat[13];              // the text node's own transform
    bool hasMat = false;
    int center = 4;
    bool nodeGlow = false, nodeBlur = false;
    double glowAmount = 0, glowColor[4] = {1, 1, 1, 1}, glowInner = 0;
    double blurH = 0, blurV = 0;
    std::vector<SavedNode> saved;
    Row root;
    bool hasRoot = false;
    bool usesPath = false;       // an animation of the text along a path (not played here)
};

inline int centerFromName(const std::string& s)
{
    static const char* names[] = {"NearBaseline", "CenterBaseline", "FarBaseline", "NearCenter", "Center", "FarCenter", "NearTop", "CenterTop", "FarTop", "CustomOffset"};
    for (int i = 0; i < 10; ++i)
        if (s == names[i]) return i;
    return 9;
}

inline bool parseMatrix(const std::string& s, double m[13])
{
    for (int i = 0; i < 13; ++i) m[i] = 0;
    m[4] = m[5] = 1; m[9] = 0.75; m[10] = 0; m[11] = 0.75; m[12] = 1;
    const size_t a = s.find('('), b = s.find(')');
    if (a == std::string::npos || b == std::string::npos || b <= a + 1) return false;
    const char* p = s.c_str() + a + 1;
    for (int i = 0; i < 13; ++i) {
        char* e = nullptr;
        const double v = std::strtod(p, &e);
        if (e == p) break;
        m[i] = v;
        p = e;
        while (*p == ',' || *p == ' ') ++p;
    }
    return true;
}

inline void parseFn(const XNode& f, Fn& o)
{
    const std::string type = f.str("xsi:type", "CompositeCurve");
    o.kind = type == "Oscillator" ? 1 : (type == "ParabolicEvaluator" ? 2 : (type == "NoiseEvaluator" ? 3 : 0));
    o.loops = f.num("loops", 1);
    o.decay = f.num("loopdecay", 1);
    o.offset = f.num("offset", 0);
    o.inner = f.num("inneroffset", 0);
    o.steps = f.num("steps", 0);
    o.autoSub = f.flag("autosubtract", false);
    o.continuous = f.flag("continuous", false);
    if (o.kind == 0) {
        if (const XNode* pts = f.child("points"))
            for (const XNode& e : pts->kids) {
                if (e.tag != "element") continue;
                ControlValue c;
                if (const XNode* k = e.child("key"))
                    if (const XNode* d = k->child("double")) c.key = std::atof(d->text.c_str());
                if (const XNode* v = e.child("value"))
                    if (const XNode* cv = v->child("ControlValue")) {
                        c.value = cv->num("value", 0);
                        const std::string t = cv->str("type", "Auto");
                        c.type = t == "Adjustable" ? 1 : (t == "SplitAdjustable" ? 2 : (t == "Linear" ? 3 : (t == "Fast" ? 4 : (t == "Slow" ? 5 : (t == "Smooth" ? 6 : 0)))));
                        c.in = cv->num("i", 0);
                        c.out = cv->num("o", 0);
                    }
                o.pts.push_back(c);
            }
        std::sort(o.pts.begin(), o.pts.end(), [](const ControlValue& a, const ControlValue& b) { return a.key < b.key; });
    } else if (o.kind == 1) {
        o.freq = std::atof(f.childText("Frequency", "0").c_str());
        o.damp = std::atof(f.childText("Damping", "0").c_str());
        o.amp = std::atof(f.childText("Amplitude", "0").c_str());
    } else if (o.kind == 3) {
        o.nfreq = std::atof(f.childText("Frequency", "10").c_str());
        o.noct = std::atof(f.childText("Octaves", "3").c_str());
        o.seed((int)std::atof(f.childText("Seed", "42").c_str()));
    }
}

inline void parseBlock(const XNode& s, Block& b)
{
    b.root = s.flag("rootblock", false);
    b.start = s.num("start", 0);
    b.length = s.num("length", 1);
    b.scale = s.num("scale", 1);
    const std::string c = s.str("combine", "Join");
    b.combine = c == "Add" ? 1 : (c == "Multiply" ? 2 : 0);
    if (const XNode* f = s.child("function")) { b.hasFn = true; parseFn(*f, b.fn); }
    for (const XNode& k : s.kids)
        if (k.tag == "children") { b.kids.emplace_back(); parseBlock(k, b.kids.back()); }
}

inline void parseRow(const XNode& r, Row& o, int depth = 0)
{
    o.binding = r.str("binding");
    o.cascadeable = r.flag("cascadeable", false);
    o.isValue = r.str("xsi:type") == "ValueRow";
    o.initial = r.num("initial", 0);
    if (const XNode* c = r.child("cascade")) {
        o.cascadeOn = c->flag("enabled", false);
        o.cascadeDur = c->num("duration", 0.2);
        o.alias = c->str("alias");
        if (const XNode* ds = c->child("delay-shape"))
            if (ds->get("loops") || ds->child("points")) { o.hasShape = true; parseFn(*ds, o.shape); }
    }
    if (o.isValue)
        if (const XNode* s = r.child("signal")) { o.hasSignal = true; parseBlock(*s, o.signal); }
    if (depth > 40) return;
    for (const XNode& k : r.kids)
        if (k.tag == "children") { o.kids.emplace_back(); parseRow(k, o.kids.back(), depth + 1); }
}

inline void parseItem(const XNode& root, Item& it)
{
    it.name = root.str("description");
    const XNode* eff = root.child("effect");
    if (!eff) return;
    if (const XNode* tn = eff->child("textnode")) {
        if (const XNode* t = tn->child("Transform")) {
            it.hasMat = parseMatrix(t->str("matrix"), it.mat);
            it.center = centerFromName(t->str("center", "Center"));
        }
        for (const XNode& e : tn->kids) {
            if (e.tag != "effects") continue;
            const std::string ty = e.str("xsi:type");
            const bool on = e.childText("Enabled") == "true";
            if (ty == "Glow") {
                it.nodeGlow = on;
                it.glowAmount = std::atof(e.childText("Amount", "0").c_str());
                it.glowInner = e.childText("InnerGlow") == "true" ? 1 : 0;
                if (const XNode* c = e.child("Color")) {
                    const std::string cs = c->str("color");
                    const char* p = cs.c_str();
                    for (int i = 0; i < 4; ++i) { char* q = nullptr; const double v = std::strtod(p, &q); if (q == p) break; it.glowColor[i] = v; p = q; while (*p == ',' || *p == ' ') ++p; }
                }
            } else if (ty == "GaussianBlur") {
                it.nodeBlur = on;
                it.blurH = std::atof(e.childText("HorizontalBlur", "0").c_str());
                it.blurV = std::atof(e.childText("VerticalBlur", "0").c_str());
            }
        }
        for (const XNode& n : tn->kids) {
            if (n.tag != "nodes") continue;
            SavedNode s;
            const std::string ty = n.str("xsi:type");
            s.kind = ty.find("Line") != std::string::npos ? "Line" : (ty.find("Word") != std::string::npos ? "Word" : "Char");
            s.index = (int)n.num("textindex", 0);
            if (const XNode* t = n.child("transform")) {
                s.hasMat = parseMatrix(t->str("matrix"), s.mat);
                s.center = centerFromName(t->str("center", "CustomOffset"));
            }
            if (const XNode* o = n.child("opacity")) s.opacity = std::atof(o->text.c_str());
            int k = 0;
            for (const XNode& e : n.kids) {
                if (e.tag != "effects") continue;
                const bool on = e.childText("Enabled") == "true";
                if (k < 3) {
                    s.fx[k] = on;
                    s.fxAmount[k] = std::atof(e.childText("Amount", "0").c_str());
                }
                if (k == 0) {
                    s.hblur = std::atof(e.childText("HorizontalBlur", "0").c_str());
                    s.vblur = std::atof(e.childText("VerticalBlur", "0").c_str());
                }
                ++k;
            }
            it.saved.push_back(s);
        }
    }
    if (const XNode* a = eff->child("automation")) { parseRow(*a, it.root); it.hasRoot = true; }
    std::vector<const Row*> todo(1, &it.root);
    while (!todo.empty()) {
        const Row* r = todo.back();
        todo.pop_back();
        if (r->binding.find(".Path") != std::string::npos) it.usesPath = true;
        for (const Row& k : r->kids) todo.push_back(&k);
    }
}

// ------------------------------------------------------------------ finding the DLL and the item
inline bool readWholeFile(const std::string& path, std::string& out)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 64l * 1024 * 1024) { std::fclose(f); return false; }
    out.resize((size_t)n);
    const size_t got = std::fread(&out[0], 1, (size_t)n, f);
    std::fclose(f);
    return got == (size_t)n;
}

// The animation's XML documents by name ("_Bounce"): one scan of the DLL, kept. Empty when VEGAS's assembly is not on this machine.
struct Library {
    std::string blob;
    std::map<std::string, std::pair<size_t, size_t>> idx;
};

inline const Library& library()
{
    static Library L = [] {
        Library l;
        std::vector<std::string> dirs;
        if (const char* e = std::getenv("RENO_VEGAS_DIR")) dirs.push_back(e);
        dirs.push_back("C:/Program Files/VEGAS/VEGAS Pro 18.0");
        dirs.push_back("C:/Program Files/MAGIX/VEGAS Pro 18.0");
        dirs.push_back("C:/Program Files/Sony/VEGAS Pro 18.0");
        dirs.push_back("/mnt/user-data/uploads/VEGAS Pro 18.0");
        for (const std::string& d : dirs) {
            if (readWholeFile(d + "/ScriptPortal.MediaSoftware.TextGen.CoreGraphics.dll", l.blob)) break;
            l.blob.clear();
        }
        const std::string open = "<MetaTextCollectionItem ", close = "</MetaTextCollectionItem>";
        size_t pos = 0;
        while (!l.blob.empty() && (pos = l.blob.find(open, pos)) != std::string::npos) {
            const size_t e = l.blob.find(close, pos);
            if (e == std::string::npos) break;
            const size_t d = l.blob.find("description=\"", pos);
            if (d != std::string::npos && d < e) {
                const size_t b = d + 13, q = l.blob.find('"', b);
                if (q != std::string::npos && q < e) l.idx[l.blob.substr(b, q - b)] = std::make_pair(pos, e + close.size());
            }
            pos = e + close.size();
        }
        return l;
    }();
    return L;
}

// The animation called 'name' ("_Bounce"), parsed once; null if the DLL or the item is not there.
inline std::shared_ptr<const Item> findItem(const std::string& name)
{
    static std::mutex m;
    static std::map<std::string, std::shared_ptr<const Item>> cache;
    std::lock_guard<std::mutex> lk(m);
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    std::shared_ptr<const Item> out;
    const Library& L = library();
    auto f = L.idx.find(name);
    if (f != L.idx.end()) {
        XNode root;
        const char* b = L.blob.data() + f->second.first;
        const char* e = L.blob.data() + f->second.second;
        if (xmlElement(b, e, root)) {
            auto item = std::make_shared<Item>();
            parseItem(root, *item);
            if (item->hasRoot) out = item;
        }
    }
    cache[name] = out;
    return out;
}

// ------------------------------------------------------------------ the animated state of the text
// One node (the whole text, a line, a word, a character): its transform (EditableMatrix), opacity and the image effects the animations drive.
struct Sub {
    double off[2] = {0, 0}, shear[2] = {0, 0}, scale[2] = {1, 1}, cust[2] = {0, 0}, rot = 0;
    int center = 9;              // 0..8 the nine places of the box (NearBaseline .. FarTop), 9 a custom place
    double opacity = 1;
    bool blurOn = false, glowOn = false;
    double hblur = 0, vblur = 0, glow = 0, glowColor[4] = {1, 1, 1, 1}, glowInner = 0;
    double track = 0; bool trackSet = false;   // the text node's Tracking row (canvas units added to every character's advance)
    double selL = 0, selR = 1;   // the text node's selection range (the part of the text that is shown, as fractions of its characters)
    bool identity() const
    {
        return off[0] == 0 && off[1] == 0 && shear[0] == 0 && shear[1] == 0 && scale[0] == 1 && scale[1] == 1 && rot == 0;
    }
};

// The structure of the user's text: its lines, their words and the words' characters (text indices: a line break counts one).
struct Shape {
    struct Word { int textIndex = 0; std::vector<int> chars; };
    struct Line { int textIndex = 0; std::vector<Word> words; };
    std::vector<Line> lines;
};

struct State {
    Sub node;
    std::vector<Sub> line;
    std::vector<std::vector<Sub>> word;
    std::vector<std::vector<std::vector<Sub>>> chr;
};

inline void subFromSaved(const SavedNode& s, Sub& o)
{
    if (s.hasMat) {
        o.off[0] = s.mat[0]; o.off[1] = s.mat[1];
        o.cust[0] = s.mat[2]; o.cust[1] = s.mat[3];
        o.scale[0] = s.mat[4]; o.scale[1] = s.mat[5];
        o.shear[0] = s.mat[6]; o.shear[1] = s.mat[7];
        o.rot = s.mat[8];
    }
    o.center = s.hasMat ? s.center : 9;
    o.opacity = s.opacity;
    o.blurOn = s.fx[0]; o.hblur = s.hblur; o.vblur = s.vblur;
    o.glowOn = s.fx[1]; o.glow = s.fxAmount[1];
}

struct Ref {
    int level = -1;              // 0 the text, 1 line, 2 word, 3 character
    int l = 0, w = 0, c = 0;
};

inline std::vector<std::string> splitDots(const std::string& s)
{
    std::vector<std::string> v;
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find('.', a);
        if (b == std::string::npos) b = s.size();
        v.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return v;
}

inline int tokIndex(const std::string& t)
{
    return t.size() > 2 && t.front() == '[' ? std::atoi(t.c_str() + 1) : -1;
}

// The item a binding names, and where the property path starts in its tokens.
inline Ref refOf(const std::vector<std::string>& tk, size_t& propAt)
{
    Ref r;
    size_t i = 0;
    if (i + 1 < tk.size() && tk[i] == "MetaTextNodes") { r.level = 0; i += 2; }
    else return r;
    if (i + 1 < tk.size() && tk[i] == "Lines" && tokIndex(tk[i + 1]) >= 0) { r.level = 1; r.l = tokIndex(tk[i + 1]); i += 2;
        if (i + 1 < tk.size() && tk[i] == "Words" && tokIndex(tk[i + 1]) >= 0) { r.level = 2; r.w = tokIndex(tk[i + 1]); i += 2;
            if (i + 1 < tk.size() && tk[i] == "Characters" && tokIndex(tk[i + 1]) >= 0) { r.level = 3; r.c = tokIndex(tk[i + 1]); i += 2; } } }
    propAt = i;
    return r;
}

struct Eval {
    const Item& it;
    const Shape& sh;
    State& st;
    double p;
    Eval(const Item& i, const Shape& s, State& t, double pp) : it(i), sh(s), st(t), p(pp) {}

    bool valid(const Ref& r) const
    {
        if (r.level < 0) return false;
        if (r.level == 0) return true;
        if (r.l < 0 || r.l >= (int)sh.lines.size()) return false;
        if (r.level == 1) return true;
        if (r.w < 0 || r.w >= (int)sh.lines[r.l].words.size()) return false;
        if (r.level == 2) return true;
        return r.c >= 0 && r.c < (int)sh.lines[r.l].words[r.w].chars.size();
    }
    Sub* at(const Ref& r)
    {
        if (!valid(r)) return nullptr;
        switch (r.level) {
        case 0: return &st.node;
        case 1: return &st.line[r.l];
        case 2: return &st.word[r.l][r.w];
        default: return &st.chr[r.l][r.w][r.c];
        }
    }
    // the members of the list a cascade runs over (from the item 'self' on)
    std::vector<Ref> listFrom(const Row& R, const Ref& self)
    {
        std::vector<std::string> tk = splitDots(R.binding);
        std::vector<std::string> parent(tk.begin(), tk.end() - (tk.empty() ? 0 : 1));
        std::string name = parent.empty() ? std::string() : parent.back();
        std::vector<std::string> scope = parent;
        if (!R.alias.empty()) {
            size_t u = 0;
            while (u < R.alias.size() && R.alias[u] == '<') ++u;
            name = R.alias.substr(u);
            for (size_t k = 0; k < u && !scope.empty(); ++k) scope.pop_back();
        } else if (!scope.empty()) scope.pop_back();
        size_t pa = 0;
        const Ref sc = refOf(scope, pa);
        std::vector<Ref> all;
        auto addLine = [&](int l) { Ref r; r.level = 1; r.l = l; all.push_back(r); };
        auto addWord = [&](int l, int w) { Ref r; r.level = 2; r.l = l; r.w = w; all.push_back(r); };
        auto addChar = [&](int l, int w, int c) { Ref r; r.level = 3; r.l = l; r.w = w; r.c = c; all.push_back(r); };
        if (sc.level >= 0) {
            const int nl = (int)sh.lines.size();
            if (name == "Lines" && sc.level == 0) for (int l = 0; l < nl; ++l) addLine(l);
            else if (name == "Words") {
                if (sc.level == 0) { for (int l = 0; l < nl; ++l) for (int w = 0; w < (int)sh.lines[l].words.size(); ++w) addWord(l, w); }
                else if (sc.level == 1 && valid(sc)) { for (int w = 0; w < (int)sh.lines[sc.l].words.size(); ++w) addWord(sc.l, w); }
            } else if (name == "Characters") {
                if (sc.level == 0) { for (int l = 0; l < nl; ++l) for (int w = 0; w < (int)sh.lines[l].words.size(); ++w) for (int c = 0; c < (int)sh.lines[l].words[w].chars.size(); ++c) addChar(l, w, c); }
                else if (sc.level == 1 && valid(sc)) { for (int w = 0; w < (int)sh.lines[sc.l].words.size(); ++w) for (int c = 0; c < (int)sh.lines[sc.l].words[w].chars.size(); ++c) addChar(sc.l, w, c); }
                else if (sc.level == 2 && valid(sc)) { for (int c = 0; c < (int)sh.lines[sc.l].words[sc.w].chars.size(); ++c) addChar(sc.l, sc.w, c); }
            }
        }
        size_t pos = all.size();
        for (size_t i = 0; i < all.size(); ++i)
            if (all[i].level == self.level && all[i].l == self.l && all[i].w == self.w && all[i].c == self.c) { pos = i; break; }
        std::vector<Ref> out;
        if (pos == all.size()) { if (valid(self)) out.push_back(self); return out; }
        out.assign(all.begin() + pos, all.end());
        return out;
    }

    static void copyState(Sub& to, const Sub& from) { to = from; }

    void setProp(Sub& s, const std::vector<std::string>& tk, size_t at, double v)
    {
        if (at >= tk.size()) return;
        const std::string& a = tk[at];
        if (a == "OpacityLevel") s.opacity = v;
        else if (a == "Tracking") { s.track = v; s.trackSet = true; }
        else if (a == "SelectionRange" && at + 1 < tk.size()) { if (tk[at + 1] == "Left") s.selL = v; else if (tk[at + 1] == "Right") s.selR = v; }
        else if (a == "EdTransform" && at + 1 < tk.size()) {
            const std::string& b = tk[at + 1];
            if (b == "Rotation") s.rot = v;
            else if (at + 2 < tk.size()) {
                const int k = tk[at + 2] == "Y" ? 1 : 0;
                if (b == "Offset") s.off[k] = v;
                else if (b == "Scale") s.scale[k] = v;
                else if (b == "Shear") s.shear[k] = v;
            }
        } else if (a == "ImageEffects" && at + 2 < tk.size()) {
            const int n = tokIndex(tk[at + 1]);
            const std::string& f = tk[at + 2];
            if (n == 0) { if (f == "HorizontalBlur") s.hblur = v; else if (f == "VerticalBlur") s.vblur = v; }
            else if (n == 1 && f == "Amount") s.glow = v;
        }
    }

    // activeCascade: the nearest ancestor row that cascades (null: none); 'list' its targets
    void walk(const Row& R, const Row* activeCascade, const std::vector<Ref>* list)
    {
        std::vector<std::string> tk = splitDots(R.binding);
        size_t pa = 0;
        Ref self = refOf(tk, pa);
        std::vector<Ref> myList;
        const Row* myCascade = activeCascade;
        const std::vector<Ref>* myTargets = list;
        if (!R.isValue && R.cascadeOn && R.cascadeable) {
            myList = listFrom(R, self);
            // the first item's saved state goes to all of the list
            if (!myList.empty()) {
                Sub* t0 = at(myList[0]);
                if (t0) { const Sub keep = *t0; for (size_t i = 1; i < myList.size(); ++i) if (Sub* d = at(myList[i])) copyState(*d, keep); }
            }
            myCascade = &R;
            myTargets = &myList;
        } else if (R.cascadeable && !R.cascadeOn) {
            myCascade = nullptr;
            myTargets = nullptr;
        }
        if (R.isValue && R.hasSignal) {
            std::vector<Ref> single;
            const std::vector<Ref>* tg = myTargets;
            const Row* cs = myCascade;
            if (!cs || !tg) { single.push_back(self); tg = &single; cs = nullptr; }
            else if (cs == &R) cs = nullptr;
            const size_t n = tg->size();
            for (size_t j = 0; j < n; ++j) {
                Sub* s = at((*tg)[j]);
                if (!s) continue;
                double t = p;
                if (cs) {
                    const double frac = n > 1 ? (double)j / (double)(n - 1) : 0.0;
                    const double d = cs->hasShape ? cs->shape.eval(frac, 1.0) : frac;
                    t = p - d * cs->cascadeDur;
                }
                bool flag = false;
                const double v = blockEval(R.signal, t, R.initial, nullptr, &flag);
                setProp(*s, tk, pa, v);
            }
        }
        for (const Row& k : R.kids) walk(k, myCascade, myTargets);
    }
};

inline void evaluate(const Item& it, const Shape& sh, double p, State& st, int nodeMode = 1)
{
    st = State();
    if (it.hasMat && nodeMode == 1) {
        SavedNode n;
        std::memcpy(n.mat, it.mat, sizeof n.mat);
        n.hasMat = true;
        n.center = it.center;
        n.fx[0] = it.nodeBlur; n.hblur = it.blurH; n.vblur = it.blurV;
        n.fx[1] = it.nodeGlow; n.fxAmount[1] = it.glowAmount;
        subFromSaved(n, st.node);
    } else {
        st.node.center = it.center;
        st.node.blurOn = it.nodeBlur; st.node.hblur = it.blurH; st.node.vblur = it.blurV;
        st.node.glowOn = it.nodeGlow; st.node.glow = it.glowAmount;
    }
    for (int i = 0; i < 4; ++i) st.node.glowColor[i] = it.glowColor[i];
    st.node.glowInner = it.glowInner;
    const size_t nl = sh.lines.size();
    st.line.assign(nl, Sub());
    st.word.assign(nl, std::vector<Sub>());
    st.chr.assign(nl, std::vector<std::vector<Sub>>());
    for (size_t l = 0; l < nl; ++l) {
        st.word[l].assign(sh.lines[l].words.size(), Sub());
        st.chr[l].resize(sh.lines[l].words.size());
        for (size_t w = 0; w < sh.lines[l].words.size(); ++w) st.chr[l][w].assign(sh.lines[l].words[w].chars.size(), Sub());
    }
    for (const SavedNode& s : it.saved) {
        for (size_t l = 0; l < nl; ++l) {
            if (s.kind == "Line" && sh.lines[l].textIndex == s.index) subFromSaved(s, st.line[l]);
            for (size_t w = 0; w < sh.lines[l].words.size(); ++w) {
                if (s.kind == "Word" && sh.lines[l].words[w].textIndex == s.index) subFromSaved(s, st.word[l][w]);
                for (size_t c = 0; c < sh.lines[l].words[w].chars.size(); ++c)
                    if (s.kind == "Char" && sh.lines[l].words[w].chars[c] == s.index) subFromSaved(s, st.chr[l][w][c]);
            }
        }
    }
    Eval ev(it, sh, st, p);
    ev.walk(it.root, nullptr, nullptr);
}

}  // namespace vfxta
}  // namespace ofx
}  // namespace reno
