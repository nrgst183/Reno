// Small helpers shared by the parts of the .veg writer.
#include "vegwrite_internal.h"

namespace reno {
namespace vegwrite {

Templates& templates()
{
    static Templates t;
    return t;
}

const Chunk* childChunk(const Chunk& c, const char* g)
{
    for (const auto& k : c.kids)
        if (!k.isList && k.id == g) return &k;
    return nullptr;
}

const Chunk* childList(const Chunk& c, const char* g)
{
    for (const auto& k : c.kids)
        if (k.isList && k.type == g) return &k;
    return nullptr;
}

std::string bytesOf(const Bytes& b) { return std::string((const char*)b.data(), b.size()); }

// The first chunk (or list) anywhere under 'c' that 'pred' accepts.
const Chunk* findChunk(const Chunk& c, const std::function<bool(const Chunk&)>& pred)
{
    for (const auto& k : c.kids) {
        if (pred(k)) return &k;
        if (k.isList)
            if (const Chunk* f = findChunk(k, pred)) return f;
    }
    return nullptr;
}

int curveToVegas(int c)
{
    switch (c) {
    case CurveFast: return 2;
    case CurveSlow: return -2;
    case CurveSmooth: return 4;
    case CurveSharp: return -4;
    case CurveHold: return 0;
    default: return 1;
    }
}

bool samePanCrop(const std::vector<PanCropKey>& a, const std::vector<PanCropKey>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].time != b[i].time || a[i].cx != b[i].cx || a[i].cy != b[i].cy || a[i].w != b[i].w || a[i].h != b[i].h ||
            a[i].rotation != b[i].rotation || a[i].curve != b[i].curve || a[i].pivotX != b[i].pivotX || a[i].pivotY != b[i].pivotY ||
            a[i].smoothness != b[i].smoothness)
            return false;
    return true;
}

bool samePoints(const Envelope& a, const Envelope& b)
{
    if (a.kind != b.kind || a.points.size() != b.points.size()) return false;
    for (size_t i = 0; i < a.points.size(); ++i)
        if (a.points[i].time != b.points[i].time || a.points[i].value != b.points[i].value || a.points[i].curve != b.points[i].curve) return false;
    return true;
}

std::string jsonText(const Json& j) { return j.isString() ? j.asString() : j.dump(); }

bool sameFxState(const FxSlot& a, const FxSlot& b)
{
    return a.kind == b.kind && a.pluginId == b.pluginId && a.bypass == b.bypass && a.beforePanCrop == b.beforePanCrop && a.audio.uid == b.audio.uid &&
           a.audio.format == b.audio.format && jsonText(a.state) == jsonText(b.state);
}

// A path as VEGAS stores it (Windows): Reno's paths already are on Windows; elsewhere they are written unchanged.
std::string vegasPath(const std::string& p)
{
#ifdef _WIN32
    std::string s = p;
    for (auto& c : s) if (c == '/') c = '\\';
    return s;
#else
    return p;
#endif
}

std::string fileNameOf(const std::string& p)
{
    const size_t sep = p.find_last_of("\\/");
    return sep == std::string::npos ? p : p.substr(sep + 1);
}

std::string stemOf(const std::string& p)
{
    const std::string f = fileNameOf(p);
    const size_t dot = f.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? f : f.substr(0, dot);
}

std::string extOf(const std::string& p)
{
    const std::string f = fileNameOf(p);
    const size_t dot = f.find_last_of('.');
    return dot == std::string::npos ? std::string() : toLower(f.substr(dot));
}

bool isStill(const MediaItem& m)
{
    static const char* const exts[] = {".jpg", ".jpeg", ".png", ".bmp", ".tif", ".tiff", ".tga", ".psd", ".dds", ".webp", ".gif", ".jfif", ".exr"};
    for (const char* e : exts)
        if (endsWithNoCase(m.path, e)) return true;
    return m.info.isStill;
}

bool splitMedia(const Bytes& b, MediaParts& out)
{
    const uint32_t hs = b.u32(0), pl = b.u32(0x24), t1 = b.u32(0x28), t2 = b.u32(0x2c), nl = b.u32(0x40);
    const size_t gap = (size_t)t1 + t2;
    if (hs != 0x80 || t1 > 0x10000 || t2 > 0x10000 || !b.has(hs, (size_t)pl + gap + nl)) return false;
    out.head = bytesOf(b.sub(0, hs));
    out.path = bytesOf(b.sub(hs, pl));
    out.gap = bytesOf(b.sub(hs + pl, gap));
    out.name = bytesOf(b.sub(hs + pl + gap, nl));
    out.rest = bytesOf(b.sub(hs + pl + gap + nl, b.size() - (hs + pl + gap + nl)));
    return true;
}
std::string joinMedia(MediaParts m)
{
    setU32(m.head, 0x24, (uint32_t)m.path.size());
    setU32(m.head, 0x40, (uint32_t)m.name.size());
    return m.head + m.path + m.gap + m.name + m.rest;
}


}  // namespace vegwrite
}  // namespace reno
