// The few GDI+ calls VEGAS's Starburst and Defocus make to draw their kernels, made the same way by Reno on Windows (flat C API of gdiplus.dll, loaded when first needed so Reno still runs
// where it is missing). Elsewhere every function returns false and the callers draw their own model of the shape. Nothing here keeps a GDI+ object after the call, so any thread may use it.
#pragma once
#include <cstdint>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mutex>
#endif

namespace reno {
namespace ofx {
namespace builtin {
namespace more {
namespace gdip {

struct PointF { float x, y; };

// One polygon filled with a path gradient brush whose path is the polygon itself (the first vertex repeated at the end): the brush's centre point is the origin, its centre colour 'centre',
// its surround colour black; the polygon is filled once per entry of 'fills' (the first fills[k] vertices). 'translations' is how many times the graphics' origin was moved to the
// middle of the bitmap by the time of the fill (the generator moves it again for the last burst, which reuses the graphics of the one before).
struct Shape {
    std::vector<PointF> pts;
    uint32_t centre = 0;
    std::vector<int> fills;
    int translations = 1;
    bool freshGraphics = true;      // false: draws on the graphics (and so the origin) of the previous shape
};

#ifdef _WIN32
namespace detail {

typedef int(WINAPI* PStartup)(ULONG_PTR*, const void*, void*);
typedef int(WINAPI* PCreateBitmap)(int, int, int, int, BYTE*, void**);
typedef int(WINAPI* PGetContext)(void*, void**);
typedef int(WINAPI* PClear)(void*, UINT32);
typedef int(WINAPI* PIntArg)(void*, int);
typedef int(WINAPI* PTranslate)(void*, float, float, int);
typedef int(WINAPI* PCreatePath)(int, void**);
typedef int(WINAPI* PAddLine2)(void*, const PointF*, int);
typedef int(WINAPI* PAddEllipse)(void*, float, float, float, float);
typedef int(WINAPI* PBrushFromPath)(void*, void**);
typedef int(WINAPI* PCenterPoint)(void*, const PointF*);
typedef int(WINAPI* PColor)(void*, UINT32);
typedef int(WINAPI* PGetCount)(void*, int*);
typedef int(WINAPI* PSurround)(void*, const UINT32*, int*);
typedef int(WINAPI* PPreset)(void*, const UINT32*, const float*, int);
typedef int(WINAPI* PFillPolygon)(void*, void*, const PointF*, int, int);
typedef int(WINAPI* PFillEllipseI)(void*, void*, int, int, int, int);
typedef int(WINAPI* PHandle)(void*);

struct Api {
    bool ok = false;
    PCreateBitmap createBitmap = nullptr;
    PGetContext getContext = nullptr;
    PClear clear = nullptr;
    PIntArg setCompositing = nullptr;
    PTranslate translate = nullptr;
    PCreatePath createPath = nullptr;
    PAddLine2 addLine2 = nullptr;
    PAddEllipse addEllipse = nullptr;
    PBrushFromPath brushFromPath = nullptr;
    PCenterPoint setCenterPoint = nullptr;
    PColor setCenterColor = nullptr;
    PGetCount getPointCount = nullptr;
    PSurround setSurround = nullptr;
    PPreset setPreset = nullptr;
    PFillPolygon fillPolygon = nullptr;
    PFillEllipseI fillEllipseI = nullptr;
    PIntArg flush = nullptr;
    PHandle deleteGraphics = nullptr, deleteBrush = nullptr, deletePath = nullptr, disposeImage = nullptr;
};

template <class T>
inline bool load(HMODULE m, const char* name, T& fn)
{
    fn = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name)));
    return fn != nullptr;
}

// gdiplus.dll started once for the process (the way the plug-in does it: version 1, nothing else set) and never shut down. nullptr when it is missing.
inline const Api* api()
{
    static Api A;
    static std::once_flag once;
    std::call_once(once, [] {
        HMODULE m = LoadLibraryW(L"gdiplus.dll");
        if (!m) return;
        PStartup startup = nullptr;
        if (!load(m, "GdiplusStartup", startup)) return;
        struct { UINT32 version; void* cb; BOOL noThread; BOOL noCodecs; } in = {1, nullptr, FALSE, FALSE};
        ULONG_PTR token = 0;
        if (startup(&token, &in, nullptr) != 0) return;
        bool good = true;
        good &= load(m, "GdipCreateBitmapFromScan0", A.createBitmap);
        good &= load(m, "GdipGetImageGraphicsContext", A.getContext);
        good &= load(m, "GdipGraphicsClear", A.clear);
        good &= load(m, "GdipSetCompositingMode", A.setCompositing);
        good &= load(m, "GdipTranslateWorldTransform", A.translate);
        good &= load(m, "GdipCreatePath", A.createPath);
        good &= load(m, "GdipAddPathLine2", A.addLine2);
        good &= load(m, "GdipAddPathEllipse", A.addEllipse);
        good &= load(m, "GdipCreatePathGradientFromPath", A.brushFromPath);
        good &= load(m, "GdipSetPathGradientCenterPoint", A.setCenterPoint);
        good &= load(m, "GdipSetPathGradientCenterColor", A.setCenterColor);
        good &= load(m, "GdipGetPathGradientPointCount", A.getPointCount);
        good &= load(m, "GdipSetPathGradientSurroundColorsWithCount", A.setSurround);
        good &= load(m, "GdipSetPathGradientPresetBlend", A.setPreset);
        good &= load(m, "GdipFillPolygon", A.fillPolygon);
        good &= load(m, "GdipFillEllipseI", A.fillEllipseI);
        good &= load(m, "GdipFlush", A.flush);
        good &= load(m, "GdipDeleteGraphics", A.deleteGraphics);
        good &= load(m, "GdipDeleteBrush", A.deleteBrush);
        good &= load(m, "GdipDeletePath", A.deletePath);
        good &= load(m, "GdipDisposeImage", A.disposeImage);
        A.ok = good;
    });
    return A.ok ? &A : nullptr;
}

// A square 32bppARGB bitmap (PixelFormat32bppARGB = 0x26200A) over memory of our own, zeroed, with a graphics for it.
struct Canvas {
    const Api* A = nullptr;
    int n = 0;
    std::vector<uint32_t> mem;
    void* bitmap = nullptr;
    void* g = nullptr;
    bool open(const Api* api_, int size)
    {
        A = api_;
        n = size;
        mem.assign((size_t)size * (size_t)size, 0u);
        if (A->createBitmap(size, size, size * 4, 0x26200A, reinterpret_cast<BYTE*>(mem.data()), &bitmap) != 0 || !bitmap) { bitmap = nullptr; return false; }
        if (A->getContext(bitmap, &g) != 0 || !g) { g = nullptr; return false; }
        return true;
    }
    void close()
    {
        if (g) A->deleteGraphics(g);
        if (bitmap) A->disposeImage(bitmap);
        g = nullptr;
        bitmap = nullptr;
    }
    ~Canvas() { close(); }
    // the blue byte (byte 0 of every pixel) as the plug-in reads it
    void blue(std::vector<uint8_t>& out) const
    {
        out.resize(mem.size());
        for (size_t i = 0; i < mem.size(); ++i) out[i] = (uint8_t)(mem[i] & 0xFFu);
    }
};

}  // namespace detail
#endif

inline bool available()
{
#ifdef _WIN32
    return detail::api() != nullptr;
#else
    return false;
#endif
}

// Starburst's generator: every shape is drawn into an n x n bitmap that is cleared first; the blue plane of each result is returned in 'blue' (one per shape). False when GDI+ is not there
// or a call failed (the caller then draws the model).
inline bool drawGradientShapes(int n, const std::vector<Shape>& shapes, std::vector<std::vector<uint8_t> >& blue)
{
#ifdef _WIN32
    const detail::Api* A = detail::api();
    if (!A || n < 1 || shapes.empty()) return false;
    blue.assign(shapes.size(), std::vector<uint8_t>());
    detail::Canvas cv;
    bool haveCanvas = false;
    for (size_t k = 0; k < shapes.size(); ++k) {
        const Shape& sh = shapes[k];
        if (sh.pts.size() < 4) return false;
        if (sh.freshGraphics || !haveCanvas) {
            cv.close();
            if (!cv.open(A, n)) return false;
            haveCanvas = true;
        }
        void* g = cv.g;
        A->clear(g, 0u);
        A->setCompositing(g, 0);
        const int times = sh.freshGraphics ? 1 : sh.translations - 1;
        for (int t = 0; t < times; ++t) A->translate(g, (float)n * 0.5f, (float)n * 0.5f, 0);
        void* path = nullptr;
        void* brush = nullptr;
        if (A->createPath(0, &path) != 0 || !path) return false;
        A->addLine2(path, sh.pts.data(), (int)sh.pts.size());
        if (A->brushFromPath(path, &brush) != 0 || !brush) { A->deletePath(path); return false; }
        const PointF origin = {0.f, 0.f};
        A->setCenterPoint(brush, &origin);
        A->setCenterColor(brush, sh.centre);
        int count = 0;
        A->getPointCount(brush, &count);
        if (count >= 1) {
            UINT32 black = 0xFF000000u;
            int one = 1;
            A->setSurround(brush, &black, &one);
        }
        for (size_t f = 0; f < sh.fills.size(); ++f) A->fillPolygon(g, brush, sh.pts.data(), sh.fills[f], 0);
        A->flush(g, 0);
        A->deleteBrush(brush);
        A->deletePath(path);
        cv.blue(blue[k]);
    }
    return true;
#else
    (void)n; (void)shapes; (void)blue;
    return false;
#endif
}

// Defocus's iris: an S x S bitmap, cleared, origin moved to the middle (S / 2), a path gradient brush from the path (an ellipse (-r, -r, 2r, 2r) when 'circle', else the n + 1 points 'poly'),
// colours 'colors' at the positions 'positions' (count entries, position 0 at the outline, 1 at the middle), then FillEllipseI (-r, -r, 2r, 2r) / FillPolygon of the first polyCount points.
// The blue plane is returned (S x S).
inline bool drawIris(int S, bool circle, float r, const std::vector<PointF>& poly, int polyCount, const uint32_t* colors, const float* positions, int count, std::vector<uint8_t>& blue)
{
#ifdef _WIN32
    const detail::Api* A = detail::api();
    if (!A || S < 1) return false;
    detail::Canvas cv;
    if (!cv.open(A, S)) return false;
    void* g = cv.g;
    A->clear(g, 0u);
    A->translate(g, (float)S * 0.5f, (float)S * 0.5f, 0);
    void* path = nullptr;
    void* brush = nullptr;
    if (A->createPath(0, &path) != 0 || !path) return false;
    if (circle) A->addEllipse(path, -r, -r, r + r, r + r);
    else A->addLine2(path, poly.data(), (int)poly.size());
    if (A->brushFromPath(path, &brush) != 0 || !brush) { A->deletePath(path); return false; }
    A->setPreset(brush, colors, positions, count);
    if (circle) A->fillEllipseI(g, brush, (int)(-r), (int)(-r), (int)(r + r), (int)(r + r));
    else A->fillPolygon(g, brush, poly.data(), polyCount, 0);
    A->flush(g, 0);
    A->deleteBrush(brush);
    A->deletePath(path);
    cv.blue(blue);
    return true;
#else
    (void)S; (void)circle; (void)r; (void)poly; (void)polyCount; (void)colors; (void)positions; (void)count; (void)blue;
    return false;
#endif
}

}  // namespace gdip
}  // namespace more
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
