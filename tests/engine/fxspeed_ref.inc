// The built-in effects as they were before the speed-ups (copied from builtins_more.cpp / builtins_filters.cpp at that
// time, slow and plain), kept as the reference the fast versions are compared with (fxspeed_test.cpp includes this).
namespace ref {
using namespace reno::ofx::builtin;
using namespace reno::ofx;
template <class F>
void perPixel(Ctx& c, F fn)
{
    const int W = c.out.w, H = c.out.h;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float r, g, b, a;
            unpremul(c.src.at(x, y), r, g, b, a);
            if (a <= 0.f) { std::memset(c.out.at(x, y), 0, sizeof(float) * 4); continue; }
            fn(r, g, b, x, y);
            premul(c.out.at(x, y), r, g, b, a);
        }
    });
}

void copyThrough(Ctx& c) { c.out.px = c.src.px; }

// out = src + (effect - src) x k
void blendOver(Ctx& c, const Buf& effect, float k)
{
    if (k >= 0.999f) { c.out.px = effect.px; return; }
    for (size_t i = 0; i < c.out.px.size(); ++i) c.out.px[i] = c.src.px[i] + (effect.px[i] - c.src.px[i]) * k;
}

void rgbToHsl(float r, float g, float b, float& h, float& s, float& l)
{
    const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    l = (mx + mn) * 0.5f;
    if (mx - mn < 1e-6f) { h = s = 0; return; }
    const float d = mx - mn;
    s = l > 0.5f ? d / std::max(1e-6f, 2.f - mx - mn) : d / std::max(1e-6f, mx + mn);
    if (mx == r) h = (g - b) / d + (g < b ? 6.f : 0.f);
    else if (mx == g) h = (b - r) / d + 2.f;
    else h = (r - g) / d + 4.f;
    h /= 6.f;
}
float hue2rgb(float p, float q, float t)
{
    t -= std::floor(t);
    if (t < 1.f / 6) return p + (q - p) * 6 * t;
    if (t < 0.5f) return q;
    if (t < 2.f / 3) return p + (q - p) * (2.f / 3 - t) * 6;
    return p;
}
void hslToRgb(float h, float s, float l, float& r, float& g, float& b)
{
    if (s <= 0) { r = g = b = l; return; }
    const float q = l < 0.5f ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    r = hue2rgb(p, q, h + 1.f / 3);
    g = hue2rgb(p, q, h);
    b = hue2rgb(p, q, h - 1.f / 3);
}

// sRGB <-> CIE Lab (D65), L 0..100
float srgbToLin(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }
float linToSrgb(float v) { return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(std::max(0.f, v), 1.f / 2.4f) - 0.055f; }
void rgbToLab(float r, float g, float b, float& L, float& A, float& B)
{
    const float lr = srgbToLin(r), lg = srgbToLin(g), lb = srgbToLin(b);
    float X = (0.4124f * lr + 0.3576f * lg + 0.1805f * lb) / 0.95047f;
    float Y = 0.2126f * lr + 0.7152f * lg + 0.0722f * lb;
    float Z = (0.0193f * lr + 0.1192f * lg + 0.9505f * lb) / 1.08883f;
    auto f = [](float t) { return t > 0.008856f ? std::cbrt(t) : 7.787f * t + 16.f / 116.f; };
    X = f(X); Y = f(Y); Z = f(Z);
    L = 116.f * Y - 16.f; A = 500.f * (X - Y); B = 200.f * (Y - Z);
}
void labToRgb(float L, float A, float B, float& r, float& g, float& b)
{
    float Y = (L + 16.f) / 116.f, X = Y + A / 500.f, Z = Y - B / 200.f;
    auto fi = [](float t) { const float t3 = t * t * t; return t3 > 0.008856f ? t3 : (t - 16.f / 116.f) / 7.787f; };
    X = fi(X) * 0.95047f; Y = fi(Y); Z = fi(Z) * 1.08883f;
    const float lr = 3.2406f * X - 1.5372f * Y - 0.4986f * Z, lg = -0.9689f * X + 1.8758f * Y + 0.0415f * Z, lb = 0.0557f * X - 0.2040f * Y + 1.0570f * Z;
    r = linToSrgb(lr); g = linToSrgb(lg); b = linToSrgb(lb);
}


double frameScale(const Ctx& c) { return std::max(1, c.out.h) / 1080.0; }


void defocus(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const double radius = std::max(0.0, 3.2714 * c.d("Radius") - 1.2651) * c.out.h / 180.0;
    if (radius < 0.5) { copyThrough(c); return; }
    const int shape = c.i("IrisShape");
    const double turn = c.d("Orientation") * 2 * kPi;
    const double curvature = clampd(c.d("DiscCurvature") / 100.0, -1, 1);
    const double th = clampd(c.d("BloomThreshold") / 100.0, 0, 2), gA = std::max(0.0, 1.1018 * c.d("BloomStrength") - 1.9834);
    const int sides = shape == 0 ? 0 : shape + 4;
    const int levels = curvature == 0 ? 1 : 4, Rc = (int)std::ceil(std::min(radius, 96.0)) + 2;
    std::vector<double> wl((size_t)levels + 1, 0.0);
    for (int l = 1; l <= levels; ++l) wl[(size_t)l] = levels == 1 ? 1.0 : std::max(0.05, 1.0 + 0.1 * curvature * (1.0 - 2 * std::sqrt((l - 0.5) / levels)));
    // the weight of each offset, and the gained frame
    std::vector<double> wt((size_t)(2 * Rc + 1) * (2 * Rc + 1), 0.0);
    double total = 0;
    for (int dy = -Rc; dy <= Rc; ++dy)
        for (int dx = -Rc; dx <= Rc; ++dx) {
            const double ux = dx + 0.5, uy = dy - 0.5, d = std::sqrt(ux * ux + uy * uy);
            for (int l = 1; l <= levels; ++l) {
                double lim = std::sqrt((double)l / levels) * std::min(radius, 96.0);
                if (sides && d > 0) {
                    const double seg = 2 * kPi / sides, ang = std::atan2(uy, ux) + kPi * 0.5 - turn;
                    lim *= std::cos(kPi / sides) / std::cos(std::fmod(std::fabs(ang), seg) - kPi / sides);
                }
                if (d <= lim + 1e-9) { wt[(size_t)(dy + Rc) * (2 * Rc + 1) + (size_t)(dx + Rc)] = wl[(size_t)l]; break; }
            }
            total += wt[(size_t)(dy + Rc) * (2 * Rc + 1) + (size_t)(dx + Rc)];
        }
    if (!(total > 0)) { copyThrough(c); return; }
    auto gained = [&](int xs, int ys, float* q) {
        if (ys < 0) ys = 0; else if (ys >= H) ys = std::min(H - 1, ys - H);
        if (xs < 0) xs = std::max(0, W + xs); else if (xs >= W) xs = W - 1;
        const float* p = c.src.at(xs, ys);
        for (int k = 0; k < 4; ++k) q[k] = p[k];
        if (gA > 0 && p[3] > 1e-4f)
            for (int k = 0; k < 3; ++k) {
                const double w = (p[k] / p[3] - th) / (th < 0.999 ? 1.0 - th : 1e9);
                if (w > 0) q[k] *= (float)(1.0 + gA * std::min(1.0, w));
            }
    };
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            double acc[4] = {0, 0, 0, 0};
            for (int dy = -Rc; dy <= Rc; ++dy)
                for (int dx = -Rc; dx <= Rc; ++dx) {
                    const double w = wt[(size_t)(dy + Rc) * (2 * Rc + 1) + (size_t)(dx + Rc)];
                    if (w == 0) continue;
                    float q[4];
                    gained(x + dx, y + dy, q);
                    for (int k = 0; k < 4; ++k) acc[k] += q[k] * w;
                }
            float* o = c.out.at(x, y);
            for (int k = 0; k < 4; ++k) o[k] = std::min(1.f, (float)(acc[k] / total));
            if (o[0] > o[3]) o[0] = o[3];
            if (o[1] > o[3]) o[1] = o[3];
            if (o[2] > o[3]) o[2] = o[3];
        }
    });
}


// Min and Max: the plain loop over a square or round neighbourhood of each pixel. Window as in neighbourhood() in
// builtins_more_blur.cpp: round(16 * range) pixels each way; a round one takes the pixels inside the ellipse of radii
// (16 * range + 0.5).
void neighbourhood(Ctx& c, bool maximum)
{
    const int W = c.out.w, H = c.out.h;
    const double hr = clampd(c.d("HorizontalRange"), 0, 1), vr = clampd(c.d("VerticalRange"), 0, 1);
    const double rxf = c.px(hr * 16), ryf = c.px(vr * 16);
    const int RX = (int)std::lround(rxf), RY = (int)std::lround(ryf);
    const bool round = c.b("CircularAreaProcessing");
    const double ax = rxf + 0.5, ay = ryf + 0.5;
    if (RX == 0 && RY == 0) { copyThrough(c); return; }
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float v[4] = {0, 0, 0, 0};
            bool first = true;
            for (int j = -RY; j <= RY; ++j)
                for (int i = -RX; i <= RX; ++i) {
                    if (round) {
                        const double u = ax > 0 ? (double)i / ax : 0, w = ay > 0 ? (double)j / ay : 0;
                        if (u * u + w * w > 1.0 + 1e-9) continue;
                    }
                    const int xx = std::min(W - 1, std::max(0, x + i)), yy = std::min(H - 1, std::max(0, y + j));
                    const float* p = c.src.at(xx, yy);
                    for (int k = 0; k < 4; ++k) v[k] = first ? p[k] : (maximum ? std::max(v[k], p[k]) : std::min(v[k], p[k]));
                    first = false;
                }
            float* o = c.out.at(x, y);
            for (int k = 0; k < 4; ++k) o[k] = v[k];
            for (int k = 0; k < 3; ++k) o[k] = std::min(std::max(o[k], 0.f), o[3]);
        }
    });
}
void minAndMax(Ctx& c) { neighbourhood(c, c.i("Operation") == 1); }


// Black Bar Fill: the current VEGAS-matched model (builtins_more_creative.cpp) with the blurs on the full-size grid.
void blackBarFill(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    double fx, fy, bx, by;
    c.xy("Foreground Location", fx, fy);
    c.xy("Background Location", bx, by);
    const double fs = std::max(0.01, c.d("Foreground Scale")), bs = std::max(0.01, c.d("Background Scale"));
    // source position (index units, row from the top) of output pixel (x, y)
    auto srcPos = [&](int x, int y, double cx, double cy, double z, double& sx, double& sy) {
        sx = (x - cx * W) / z + W * 0.5;
        const double sj = ((H - 1 - y) - cy * H) / z + H * 0.5;
        sy = (H - 1) - sj;
    };
    const double eps = 0.397;
    auto inside = [&](double sx, double sy) { return sx >= -eps && sx <= W - 1 + eps && sy >= -eps && sy <= H - 1 + eps; };
    auto isWhole = [&](double z, double cx, double cy) {
        double sx, sy;
        srcPos(0, 0, cx, cy, z, sx, sy);
        return std::fabs(z - 1) < 1e-9 && std::fabs(sx - std::floor(sx + 0.5)) < 1e-6 && std::fabs(sy - std::floor(sy + 0.5)) < 1e-6;
    };
    // bilinear on the index grid; nothing outside the frame
    auto fetch = [&](double sx, double sy, float* o) {
        sx = clampd(sx, 0, W - 1);
        sy = clampd(sy, 0, H - 1);
        const int x0 = std::min((int)sx, W - 1), y0 = std::min((int)sy, H - 1), x1 = std::min(x0 + 1, W - 1), y1 = std::min(y0 + 1, H - 1);
        const float u = (float)(sx - x0), v = (float)(sy - y0);
        const float *a = c.src.at(x0, y0), *b = c.src.at(x1, y0), *cc = c.src.at(x0, y1), *d = c.src.at(x1, y1);
        for (int i = 0; i < 4; ++i) o[i] = (a[i] * (1 - u) + b[i] * u) * (1 - v) + (cc[i] * (1 - u) + d[i] * u) * v;
    };
    // The background: an enlarged copy, blurred (sigma 0.32 W per unit of Blur, a true Gaussian: made on a reduced grid
    // only for a very big blur).
    Buf bg;
    bg.resize(W, H);
    const double blur = clampd(c.d("Background Blur"), 0, 1) * 0.3213 * W;
    const bool bgWhole = isWhole(bs, bx, by);
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            double sx, sy;
            srcPos(x, y, bx, by, bs, sx, sy);
            float* o = bg.at(x, y);
            if (inside(sx, sy)) fetch(sx, sy, o); else std::memset(o, 0, sizeof(float) * 4);
        }
    });
    const bool bgBlurred = blur > c.px(0.05);
    if (bgBlurred) {
        gaussianBlur(bg, blur, blur);          // (the original: on the full-size grid)
    }
    if (c.levels && (bgBlurred || !bgWhole)) {          // what VEGAS's resampling/blur does to 8-bit levels
        forRows(H, W, [&](int y) {
            for (int x = 0; x < W; ++x) {
                float* o = bg.at(x, y);
                for (int i = 0; i < 3; ++i) {
                    const double v = o[i] * 255.0;
                    o[i] = (float)(bgBlurred ? std::max(0.0, std::floor(v * (254.0 / 255.0) - 0.5 + 1e-4)) : std::floor(std::ceil(v - 1e-3) * (127.0 / 128.0) + 1e-6)) / 255.f;
                }
                o[3] = (float)std::max(0.0, std::ceil(o[3] * 255.0 - 1e-3) - (o[3] > 0 ? 1 : 0)) / 255.f;
            }
        });
    }
    // the foreground: where it exists (a hard mask) and its pixels
    const bool fgWhole = isWhole(fs, fx, fy);
    std::vector<unsigned char> fm((size_t)W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            double sx, sy;
            srcPos(x, y, fx, fy, fs, sx, sy);
            fm[(size_t)y * W + x] = inside(sx, sy) ? 1 : 0;
        }
    const double shadow = clampd(c.d("Foreground Shadow Blur"), 0, 1);
    std::vector<float> sh;
    if (shadow > 0.001) {
        const double sigma = shadow * 0.029375 * W;
        sh.assign((size_t)W * H, 0.f);
        for (size_t i = 0; i < (size_t)W * H; ++i) sh[i] = (float)fm[i];
        gaussianBlurPlane(sh, W, H, sigma, sigma);   // (the original: on the full-size grid)
    }
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float* o = c.out.at(x, y);
            std::memcpy(o, bg.at(x, y), sizeof(float) * 4);
            if (!sh.empty()) {
                const float k = clamp01(sh[(size_t)y * W + x]);
                float v[3] = {o[0] * (1 - k), o[1] * (1 - k), o[2] * (1 - k)};
                for (int i = 0; i < 3; ++i) o[i] = c.levels ? std::floor(v[i] * 255.f + 1e-3f) / 255.f : v[i];
            }
            if (!fm[(size_t)y * W + x]) continue;
            float p[4];
            double sx, sy;
            srcPos(x, y, fx, fy, fs, sx, sy);
            fetch(sx, sy, p);
            if (c.levels && !fgWhole)
                for (int i = 0; i < 3; ++i) p[i] = (float)std::floor(std::ceil(p[i] * 255.0 - 1e-3) * (127.0 / 128.0) + 1e-6) / 255.f;
            for (int i = 0; i < 4; ++i) o[i] = p[i] + o[i] * (1 - p[3]);
        }
    });
}


// Light Rays: the bright parts streak away from the light source.
void lightRays(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    double lx, ly;
    c.xy("LightSource", lx, ly);
    const Color tint = c.color("TintColor");
    const double sens = clampd(c.d("group0Sensitivity"), 0, 1), strength = c.d("group0Strength") * 3;
    const double blend = clampd(c.d("group2Blend"), 0, 1);
    const double px0 = lx * W, py0 = (1 - ly) * H;
    const int taps = 40;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float acc[3] = {0, 0, 0};
            for (int t = 1; t <= taps; ++t) {
                const double f = 1.0 - (double)t / taps * 0.6;                      // marching towards the light
                float p[4];
                c.src.sampleClamped(px0 + (x + 0.5 - px0) * f, py0 + (y + 0.5 - py0) * f, p);
                const float a = p[3] > 1e-4f ? 1.f / p[3] : 0.f;
                const float l = luma(p[0] * a, p[1] * a, p[2] * a);
                const float w = std::max(0.f, l - (float)(1.0 - sens)) * (1.f - (float)t / taps);
                for (int k = 0; k < 3; ++k) acc[k] += p[k] * a * w;
            }
            const float* s = c.src.at(x, y);
            float* o = c.out.at(x, y);
            const double tc[3] = {tint.r, tint.g, tint.b};
            for (int k = 0; k < 3; ++k) o[k] = std::min(1.f, s[k] + acc[k] / taps * (float)(strength * tc[k]) * (1.f - (float)blend * 0.5f));
            o[3] = s[3];
        }
    });
}

// Rays / Glint / Starburst (the VelvetMatter effects): streaks of light from the bright points of the picture.
void streaks(Ctx& c, int arms, double length, double threshold, double boost, double angle, const Color& tint, bool effectOnly)
{
    const int W = c.out.w, H = c.out.h;
    Buf layer;
    layer.resize(W, H);
    arms = std::min(arms, 12);                                // (more arms only repeat the same streaks)
    for (int a = 0; a < arms; ++a) {
        const double ang = angle + a * kPi / arms;
        const double ca = std::cos(ang), sa = std::sin(ang);
        const int taps = 32;
        forRows(H, W, [&](int y) {
            for (int x = 0; x < W; ++x) {
                float acc[3] = {0, 0, 0};
                for (int t = -taps; t <= taps; ++t) {
                    if (t == 0) continue;
                    const double dist = t / (double)taps * length;
                    float p[4];
                    c.src.sampleClamped(x + 0.5 + ca * dist, y + 0.5 + sa * dist, p);
                    const float al = p[3] > 1e-4f ? 1.f / p[3] : 0.f;
                    const float l = luma(p[0] * al, p[1] * al, p[2] * al);
                    const float w = std::max(0.f, l - (float)threshold) * (1.f - std::fabs((float)t) / taps);
                    for (int k = 0; k < 3; ++k) acc[k] += p[k] * al * w;
                }
                float* o = layer.at(x, y);
                for (int k = 0; k < 3; ++k) o[k] += acc[k] / taps;
            }
        });
    }
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const float* l = layer.at(x, y);
            const float* s = c.src.at(x, y);
            float* o = c.out.at(x, y);
            const double tc[3] = {tint.r, tint.g, tint.b};
            const float a = std::max(s[3], 0.f);
            for (int k = 0; k < 3; ++k) {
                const float add = l[k] * (float)(boost * tc[k]);
                o[k] = effectOnly ? std::min(1.f, add) : std::min(1.f, s[k] + add);
            }
            o[3] = effectOnly ? std::min(1.f, std::max(a, (o[0] + o[1] + o[2]) / 3)) : a;
            for (int k = 0; k < 3; ++k) o[k] = std::min(o[k], o[3]);
        }
    });
}

void hsvColor(double hueDeg, double sat, Color& col)
{
    float r, g, b;
    hslToRgb((float)(hueDeg / 360.0), (float)clampd(sat, 0, 1), 0.5f, r, g, b);
    col.r = 1 - (1 - r) * sat; col.g = 1 - (1 - g) * sat; col.b = 1 - (1 - b) * sat;
}

void starburst(Ctx& c)
{
    Color white{1, 1, 1, 1};
    const double len = c.d("Radius") / 100.0 * 0.6 * c.out.w;
    streaks(c, std::max(1, c.i("Points")), len, 1.0 - c.d("Threshold") / 100.0, c.d("Strength") / 100.0 * 2.0, c.d("Orientation") * kPi / 180.0, white, c.b("EffectOnly"));
}


// Fill Light: the current VEGAS-matched curve (builtins_more_light.cpp) with the luma blur at full size.
void fillLight(Ctx& c)
{
    static const float kTab[3][32] = {
        {0.0, 2.0, 5.9, 11.1, 15.8, 18.8, 19.0, 19.0, 18.9, 17.9, 17.1, 16.0, 15.0, 13.9, 12.6, 11.1, 9.8, 8.5, 7.2, 5.7, 4.5, 3.4, 2.2, 1.5, 0.7, 0.1, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
        {0.0, 0.8, 2.8, 5.9, 9.5, 13.2, 16.3, 18.6, 19.0, 19.0, 19.0, 18.0, 17.1, 16.0, 14.7, 13.4, 11.7, 10.2, 8.8, 7.3, 5.6, 4.2, 3.1, 1.9, 1.0, 0.1, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
        {0.0, 0.5, 1.5, 3.5, 6.0, 8.7, 11.6, 14.2, 16.4, 18.2, 19.0, 19.0, 19.0, 18.2, 17.0, 15.8, 14.4, 12.7, 10.9, 9.1, 7.4, 5.6, 4.0, 2.6, 1.4, 0.4, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}};
    const double hue = c.d("Hue") / 360.0, strength = c.d("Strength") / 100.0, bias = clampd(c.d("Bias") / 100.0, -1, 1), tintAmt = clampd(c.d("Tint") / 100.0, 0, 1), soft = c.d("Softness");
    Buf blur;
    if (soft > 0.1) { blur = c.src; { const double sg = soft * 0.00037 * c.out.w; gaussianBlur(blur, sg, sg); } }
    float tr, tg, tb;
    hslToRgb((float)hue, 1.f, 0.5f, tr, tg, tb);
    const float ty = 0.299f * tr + 0.587f * tg + 0.114f * tb;
    const float tw[3] = {1.f + (float)tintAmt * 0.37f * (tr - ty), 1.f + (float)tintAmt * 0.37f * (tg - ty), 1.f + (float)tintAmt * 0.37f * (tb - ty)};
    // the curve for this Bias, 256 luma values
    float curve[256];
    {
        const int lo = bias < 0 ? 0 : 1;
        const float t = (float)(bias < 0 ? bias + 1 : bias);
        const float amp = (float)(std::pow(std::max(0.0, strength) / 0.5213, 1.04) / 0.5213 * 0.5213 / 255.0);
        for (int i = 0; i < 256; ++i) {
            const float u = (float)i / 8.f;
            const int k = std::min(30, (int)u);
            const float fr = u - (float)k;
            const float a = kTab[lo][k] + (kTab[lo][k + 1] - kTab[lo][k]) * fr, b = kTab[lo + 1][k] + (kTab[lo + 1][k + 1] - kTab[lo + 1][k]) * fr;
            curve[i] = (a + (b - a) * t) * amp;
        }
    }
    perPixel(c, [&](float& r, float& g, float& b, int x, int y) {
        float br = r, bg = g, bb = b, ba;
        if (soft > 0.1) unpremul(blur.at(x, y), br, bg, bb, ba);
        const float l = clamp01(0.299f * br + 0.587f * bg + 0.114f * bb) * 255.f;
        const int i0 = std::min(254, (int)l);
        const float lift = curve[i0] + (curve[i0 + 1] - curve[i0]) * (l - (float)i0);
        r = clamp01(r + lift * tw[0]); g = clamp01(g + lift * tw[1]); b = clamp01(b + lift * tw[2]);
    });
}


}  // namespace ref
