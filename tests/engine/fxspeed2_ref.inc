// The built-in effects as they were before the speed-ups of tests/engine/fxspeed2_test.cpp (copied from the sources at that
// time, plain and slow): the reference the fast versions must match bit for bit.
namespace ref2 {
using namespace reno;
using namespace reno::ofx::builtin;


// ------------------------------------------------------------------ Convolution Kernel
// A 3 x 3 kernel over each pixel's neighbours (turned by Rotation, in degrees: the neighbours are read at turned
// places), divided by the sum of the kernel when Auto normalize is on and the sum is not 0, else multiplied by Scale,
// then Offset added (0..1). Colour only; alpha is kept.
void convolutionKernel(Ctx& c)
{
    double k[3][3];
    for (int r = 0; r < 3; ++r)
        for (int col = 0; col < 3; ++col) k[r][col] = clampd(c.d(strfmt("Row%dColumn%d", r + 1, col + 1).c_str()), -1e4, 1e4);
    double sum = 0;
    for (auto& row : k) for (double v : row) sum += v;
    const bool norm = c.b("AutoNormalize") && std::fabs(sum) > 1e-9;
    const float mul = (float)(norm ? 1.0 / sum : clampd(c.d("Scale"), -1e4, 1e4));
    const float off = (float)clampd(c.d("Offset"), -100, 100);
    const double rot = c.d("Rotation") * kPi / 180.0;
    const double cr = std::cos(rot), sr = std::sin(rot);
    const bool turned = std::fabs(rot) > 1e-9;
    // a neighbour is one full-size pixel away: in a smaller frame, a fraction of a pixel, read between the pixels
    const bool between = c.scale < 0.999;
    // the frame straight (unpremultiplied), so dark edges of soft alpha do not bleed in
    const int W = c.out.w, H = c.out.h;
    Buf s;
    s.ensure(W, H);
    forRows(H, W, [&](int y) {
        const float* in = c.src.at(0, y);
        float* o = s.at(0, y);
        int x = 0;
        for (; x < W; ++x) {
            const float* q = in + (size_t)x * 4;
            float* d = o + (size_t)x * 4;
            const float a = q[3];
            for (int ch = 0; ch < 3; ++ch) d[ch] = a > 1e-6f ? q[ch] / a : 0.f;
            d[3] = a;
        }
    });
    // the taps that count (a zero tap adds nothing), in the plain loop's order
    struct Tap { float kv; int dx, dy; double fdx, fdy; };
    std::vector<Tap> taps;
    for (int r = 0; r < 3; ++r)
        for (int col = 0; col < 3; ++col)
            if (k[r][col] != 0) taps.push_back(Tap{(float)k[r][col], col - 1, r - 1, (double)(col - 1), (double)(r - 1)});
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float acc[3] = {0, 0, 0};
            for (const Tap& tp : taps) {
                float t[4];
                if (between) s.sampleClamped(x + 0.5 + (tp.fdx * cr - tp.fdy * sr) * c.scale, y + 0.5 + (tp.fdx * sr + tp.fdy * cr) * c.scale, t);
                else if (turned) s.sampleClamped(x + 0.5 + tp.fdx * cr - tp.fdy * sr, y + 0.5 + tp.fdx * sr + tp.fdy * cr, t);
                else {
                    const float* q = s.at(std::min(W - 1, std::max(0, x + tp.dx)), std::min(H - 1, std::max(0, y + tp.dy)));
                    t[0] = q[0]; t[1] = q[1]; t[2] = q[2];
                }
                for (int ch = 0; ch < 3; ++ch) acc[ch] += tp.kv * t[ch];
            }
            const float a = s.at(x, y)[3];
            premul(c.out.at(x, y), acc[0] * mul + off, acc[1] * mul + off, acc[2] * mul + off, a);
        }
    });
}


// Mirror
void mirror(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    double cx, cy;
    c.xy("Center", cx, cy);
    const double a = c.d("Angle") * kPi / 180.0;
    const double kx = -std::sin(a), ky = -std::cos(a);        // the kept side's direction, y down
    const double px0 = cx * W, py0 = (1 - cy) * H;
    const Color bg = c.color("Background");
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            double px = x + 0.5, py = y + 0.5;
            const double d = (px - px0) * kx + (py - py0) * ky;
            if (d < 0) { px -= 2 * d * kx; py -= 2 * d * ky; }
            float* o = c.out.at(x, y);
            if (px < 0 || py < 0 || px > W || py > H) { put(o, bg); continue; }
            c.src.sampleClamped(px, py, o);
        }
    });
}


// Mesh Warp
inline void copyFrame(Ctx& c)
{
    if (c.out.px.size() == c.src.px.size()) c.out.px = c.src.px;
}
struct Mesh {
    int cols = 0, rows = 0;
    std::vector<float> pt;      // x, y per point
    bool identity = true;
};

bool readMesh(const std::string& text, Mesh& m)
{
    const std::string b = base64Decode(text);
    if (b.size() < 16) return false;
    int32_t hdr[4];
    std::memcpy(hdr, b.data(), 16);
    if (hdr[0] < 2 || hdr[1] < 2 || hdr[0] > 200 || hdr[1] > 200) return false;
    const size_t n = (size_t)hdr[0] * (size_t)hdr[1];
    if (b.size() < 16 + n * 8) return false;
    m.cols = hdr[0];
    m.rows = hdr[1];
    m.pt.resize(n * 2);
    std::memcpy(m.pt.data(), b.data() + 16, n * 8);
    m.identity = true;
    for (int j = 0; j < m.rows; ++j)
        for (int i = 0; i < m.cols; ++i) {
            float* p = &m.pt[((size_t)j * m.cols + i) * 2];
            if (!std::isfinite(p[0]) || !std::isfinite(p[1])) return false;
            p[0] = std::min(std::max(p[0], -8.f), 9.f);
            p[1] = std::min(std::max(p[1], -8.f), 9.f);
            if (std::fabs(p[0] - (float)i / (m.cols - 1)) > 1e-4f || std::fabs(p[1] - (float)j / (m.rows - 1)) > 1e-4f) m.identity = false;
        }
    return true;
}

void meshWarp(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    Mesh m;
    if (!c.b("EnableWarp") || !readMesh(c.s("WarpData"), m) || m.identity) { copyFrame(c); return; }
    const int cols = m.cols, rows = m.rows;
    if (c.i("WarpDirection") == 0) {
        // Source: every output pixel looks up where in the frame to take its colour from, by bilinear interpolation of the
        // points of the cell it is in
        forRows(H, W, [&](int y) {
            const double v = clampd((y + 0.5) / H, 0, 1) * (rows - 1);
            const int j = std::min(rows - 2, (int)v);
            const double fy = v - j;
            for (int x = 0; x < W; ++x) {
                const double u = clampd((x + 0.5) / W, 0, 1) * (cols - 1);
                const int i = std::min(cols - 2, (int)u);
                const double fx = u - i;
                const float *a = &m.pt[((size_t)j * cols + i) * 2], *b = a + 2, *cc = a + (size_t)cols * 2, *d = cc + 2;
                const double sx = (a[0] * (1 - fx) + b[0] * fx) * (1 - fy) + (cc[0] * (1 - fx) + d[0] * fx) * fy;
                const double sy = (a[1] * (1 - fx) + b[1] * fx) * (1 - fy) + (cc[1] * (1 - fx) + d[1] * fx) * fy;
                c.src.sampleClamped(sx * W, sy * H, c.out.at(x, y));
            }
        });
        return;
    }
    // Target: the mesh's cells, each cut into two triangles, are laid at the points; a pixel inside a triangle takes the
    // colour of the same place in the regular cell's triangle. Pixels no triangle covers stay transparent.
    struct Tri { double x[3], y[3], sx[3], sy[3], den; int y0, y1; };
    std::vector<Tri> tris;
    tris.reserve((size_t)(cols - 1) * (rows - 1) * 2);
    auto add = [&](int a0, int a1, int a2) {
        Tri t;
        const int id[3] = {a0, a1, a2};
        for (int k = 0; k < 3; ++k) {
            const int i = id[k] % cols, j = id[k] / cols;
            t.x[k] = m.pt[(size_t)id[k] * 2] * W;
            t.y[k] = m.pt[(size_t)id[k] * 2 + 1] * H;
            t.sx[k] = (double)i / (cols - 1) * W;
            t.sy[k] = (double)j / (rows - 1) * H;
        }
        t.den = (t.y[1] - t.y[2]) * (t.x[0] - t.x[2]) + (t.x[2] - t.x[1]) * (t.y[0] - t.y[2]);
        if (std::fabs(t.den) < 1e-9) return;
        t.y0 = (int)std::floor(std::min(t.y[0], std::min(t.y[1], t.y[2])) - 0.5);
        t.y1 = (int)std::ceil(std::max(t.y[0], std::max(t.y[1], t.y[2])) - 0.5);
        if (t.y1 < 0 || t.y0 >= H) return;
        tris.push_back(t);
    };
    for (int j = 0; j + 1 < rows; ++j)
        for (int i = 0; i + 1 < cols; ++i) {
            const int a = j * cols + i, b = a + 1, cc = a + cols, d = cc + 1;
            add(a, b, cc);
            add(b, d, cc);
        }
    std::vector<std::vector<int>> perRow((size_t)H);
    for (size_t k = 0; k < tris.size(); ++k)
        for (int y = std::max(0, tris[k].y0); y <= std::min(H - 1, tris[k].y1); ++y) perRow[(size_t)y].push_back((int)k);
    forRows(H, W, [&](int y) {
        float* row = c.out.at(0, y);
        std::memset(row, 0, sizeof(float) * 4 * (size_t)W);
        const double yc = y + 0.5;
        for (int k : perRow[(size_t)y]) {
            const Tri& t = tris[(size_t)k];
            const double xmin = std::min(t.x[0], std::min(t.x[1], t.x[2])), xmax = std::max(t.x[0], std::max(t.x[1], t.x[2]));
            const int xa = std::max(0, (int)std::floor(xmin - 0.5)), xb = std::min(W - 1, (int)std::ceil(xmax - 0.5));
            const double eps = -1e-7;
            for (int x = xa; x <= xb; ++x) {
                const double xc = x + 0.5;
                const double w0 = ((t.y[1] - t.y[2]) * (xc - t.x[2]) + (t.x[2] - t.x[1]) * (yc - t.y[2])) / t.den;
                const double w1 = ((t.y[2] - t.y[0]) * (xc - t.x[2]) + (t.x[0] - t.x[2]) * (yc - t.y[2])) / t.den;
                const double w2 = 1 - w0 - w1;
                if (w0 < eps || w1 < eps || w2 < eps) continue;
                c.src.sampleClamped(w0 * t.sx[0] + w1 * t.sx[1] + w2 * t.sx[2], w0 * t.sy[0] + w1 * t.sy[1] + w2 * t.sy[2], row + (size_t)x * 4);
            }
        }
    });
}


// Cookie Cutter
namespace cookie {
// The 14 shapes, as VEGAS lists them.
enum Shape { Circle, Oval, OvalSide, Rectangle, Diamond, Square, Tri, TriRight, TriDown, TriLeft, Arrow, ArrowRight, ArrowDown, ArrowLeft, ShapeCount };

struct Pt { double x, y; };

bool inPolygon(const Pt* v, int n, double x, double y)
{
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        if ((v[i].y > y) != (v[j].y > y)) {
            const double xc = v[j].x + (y - v[j].y) * (v[i].x - v[j].x) / (v[i].y - v[j].y);
            if (x < xc) in = !in;
        }
    }
    return in;
}

// A shape of one cell: the point (x, y), relative to the shape's centre in pixels (y up), inside it at "radius" r?
// 'grow' is added to Rectangle's half-sides (its border works in pixels, not by radius) and 'axes' are the cell's
// width / diagonal and height / diagonal (the shapes that take the cell's shape).
struct ShapeTest {
    int shape = Circle;
    double aw = 0.7071, ah = 0.7071;    // cell width / diagonal, height / diagonal
    bool inside(double x, double y, double r, double rectGrow) const
    {
        if (r <= 0 && !(shape == Rectangle && rectGrow > 0)) return false;
        const double ax = std::fabs(x), ay = std::fabs(y);
        switch (shape) {
        case Circle: return x * x + y * y <= r * r;
        case Oval: case OvalSide: {
            const double k = shape == Oval ? std::sqrt(2.0) : 1.861;
            const double a = shape == Oval ? k * r * aw : k * r * ah, b = shape == Oval ? k * r * ah : k * r * aw;
            if (a <= 0 || b <= 0) return false;
            return (x / a) * (x / a) + (y / b) * (y / b) <= 1.0;
        }
        case Rectangle: return ax <= r * aw + rectGrow && ay <= r * ah + rectGrow;
        case Diamond: return ax + ay <= 1.3665 * r;
        case Square: return std::max(ax, ay) <= 0.8786 * r;
        default: break;
        }
        const double s3 = std::sqrt(3.0);
        Pt v[4];
        int n = 3;
        switch (shape) {
        case Tri: v[0] = {0, 2 * r}; v[1] = {s3 * r, -r}; v[2] = {-s3 * r, -r}; break;
        case TriRight: v[0] = {2 * r, 0}; v[1] = {-r, -s3 * r}; v[2] = {-r, s3 * r}; break;
        case TriDown: v[0] = {0, -2 * r}; v[1] = {-s3 * r, r}; v[2] = {s3 * r, r}; break;
        case TriLeft: v[0] = {-2 * r, 0}; v[1] = {r, s3 * r}; v[2] = {r, -s3 * r}; break;
        case Arrow: n = 4; v[0] = {0, 2 * r}; v[1] = {2 * r, -2 * r}; v[2] = {0, -r}; v[3] = {-2 * r, -2 * r}; break;
        case ArrowRight: n = 4; v[0] = {2 * r, 0}; v[1] = {-2 * r, -2 * r}; v[2] = {-r, 0}; v[3] = {-2 * r, 2 * r}; break;
        case ArrowDown: n = 4; v[0] = {0, -2 * r}; v[1] = {-2 * r, 2 * r}; v[2] = {0, r}; v[3] = {2 * r, 2 * r}; break;
        case ArrowLeft: n = 4; v[0] = {-2 * r, 0}; v[1] = {2 * r, 2 * r}; v[2] = {r, 0}; v[3] = {2 * r, -2 * r}; break;
        default: return false;
        }
        return inPolygon(v, n, x, y);
    }
    // How far from the centre the shape can reach at radius r (for the area worth looking at).
    double reach(double r, double rectGrow) const
    {
        switch (shape) {
        case Circle: return r;
        case Oval: return std::sqrt(2.0) * r * std::max(aw, ah);
        case OvalSide: return 1.861 * r * std::max(aw, ah);
        case Rectangle: return r + rectGrow * 1.5;
        case Diamond: return 1.3665 * r;
        case Square: return 0.8786 * r * 1.5;
        default: return 2.0 * r * 1.5;
        }
    }
};

// How much the inside of the border is shrunk, in radius, per pixel of border (see the top of the file).
double innerShrink(int shape)
{
    switch (shape) {
    case Circle: return 1.0;
    case Square: return 1.138;
    case Diamond: return 0.732;
    case Tri: case TriRight: case TriDown: case TriLeft: return 0.866;
    case Arrow: case ArrowRight: case ArrowDown: case ArrowLeft: return 1.12;
    default: return 1.0;
    }
}

void cookieCutter(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    const int shape = std::max(0, std::min((int)ShapeCount - 1, c.i("Shape")));
    const bool cutSection = c.i("Method") == 1;
    const double feather = clampd(c.d("Feather"), 0, 2);
    const double border = clampd(c.d("Border"), 0, 1);
    const int rx = std::max(0, std::min(100, c.i("RepeatX"))), ry = std::max(0, std::min(100, c.i("RepeatY")));
    const double size = clampd(c.d("Size"), 0, 10);
    double cx = 0.5, cy = 0.5;
    c.xy("Center", cx, cy);
    const Color col = c.color("Color");

    // the inside of the cut (I) and the inside of its border's outer edge (O), per pixel, top row first
    const size_t N = (size_t)W * H;
    std::vector<float> O(N, 0.f), I(N, 0.f);
    double sigma = 0;
    if (rx > 0 && ry > 0) {
        const double cw = (double)W / rx, ch = (double)H / ry;
        const double diag = std::hypot(cw, ch), longer = std::max(cw, ch);
        ShapeTest st;
        st.shape = shape;
        st.aw = cw / diag;
        st.ah = ch / diag;
        const double r = size * diag;
        const double T = border * longer / 2;
        double rO = r, rI = r, growO = 0, growI = 0;
        if (T > 0) {
            if (shape == Rectangle) { growO = T / 2; growI = -T / 2; }
            else if (shape == Oval || shape == OvalSide) {
                // the ovals: not grown; the inside shrunk in radius (and so in both axes alike)
                rI = r - (shape == Oval ? 0.68 : 0.515) * T;
            } else {
                rO = r * (1 + T / (2 * diag));
                rI = rO - innerShrink(shape) * T;
            }
        }
        // (VEGAS's blur grows with Feather up to 0.305 of the radius - of the border's inside, when there is a border -
        // and to 0.18 of the border's width, and no further)
        if (feather > 0) {
            sigma = std::min(0.0783 * feather * longer + c.px(0.7), 0.305 * std::max(0.0, T > 0 && shape != Rectangle ? rI : r));
            if (T > 0) sigma = std::min(sigma, 0.18 * T);
        }
        const double reach = st.reach(std::max(rO, r), growO) + 2;
        for (int ky = 0; ky < ry; ++ky)
            for (int kx = 0; kx < rx; ++kx) {
                const double px = (kx + cx) * cw, py = (ky + cy) * ch;     // y up, from the bottom row
                // (circle, ovals and rectangle stay inside their own cell: VEGAS cuts them off at the cell's
                // edges; the others reach into the cells round them)
                const bool own = shape == Circle || shape == Oval || shape == OvalSide || shape == Rectangle;
                const int cx0 = own ? (int)std::ceil(kx * cw - 1e-9) : 0, cx1 = own ? std::min(W, (int)std::ceil((kx + 1) * cw - 1e-9)) - 1 : W - 1;
                const int cj0 = own ? (int)std::ceil(ky * ch - 1e-9) : 0, cj1 = own ? std::min(H, (int)std::ceil((ky + 1) * ch - 1e-9)) - 1 : H - 1;
                const int x0 = std::max(cx0, (int)std::floor(px - reach)), x1 = std::min(cx1, (int)std::ceil(px + reach));
                const int j0 = std::max(cj0, (int)std::floor(py - reach)), j1 = std::min(cj1, (int)std::ceil(py + reach));
                for (int j = j0; j <= j1; ++j) {
                    const int y = H - 1 - j;                                  // Buf rows: top first
                    for (int x = x0; x <= x1; ++x) {
                        const double dx = x + c.shift() - px, dy = j + c.shift() - py;
                        const size_t k = (size_t)y * W + x;
                        if (O[k] < 1.f && st.inside(dx, dy, rO, growO)) O[k] = 1.f;
                        if (I[k] < 1.f && (T > 0 ? st.inside(dx, dy, rI, growI) : O[k] > 0.f)) I[k] = 1.f;
                    }
                }
            }
        if (T > 0)
            for (size_t k = 0; k < N; ++k) I[k] = std::min(I[k], O[k]);
    }
    if (sigma > 0) {
        gaussianBlurPlane(O, W, H, sigma, sigma);
        gaussianBlurPlane(I, W, H, sigma, sigma);
    }
    const float br = (float)clampd(col.r, 0, 1), bg = (float)clampd(col.g, 0, 1), bb = (float)clampd(col.b, 0, 1);
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const size_t k = (size_t)y * W + x;
            const float o = clamp01(O[k]), in = std::min(o, clamp01(I[k]));
            const float band = o - in;
            const float keep = cutSection ? 1.f - o : in;          // how much of the frame stays
            const float* s = c.src.at(x, y);
            float* d = c.out.at(x, y);
            d[0] = s[0] * keep + br * band;
            d[1] = s[1] * keep + bg * band;
            d[2] = s[2] * keep + bb * band;
            d[3] = s[3] * keep + band;
        }
    });
}
}  // namespace cookie


// Layer Dimensionality
void layerDimensionality(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const int shadowType = c.i("ShadowType"), glowType = c.i("GlowType");
    std::vector<float> alpha((size_t)W * H);
    for (size_t i = 0; i < alpha.size(); ++i) alpha[i] = c.src.px[i * 4 + 3];
    c.out.px = c.src.px;
    auto over = [&](const std::vector<float>& cover, double r, double g, double b, float k) {
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const float a = cover[(size_t)y * W + x] * k;
                if (a <= 0.f) continue;
                float* o = c.out.at(x, y);
                o[0] = (float)r * a + o[0] * (1 - a); o[1] = (float)g * a + o[1] * (1 - a); o[2] = (float)b * a + o[2] * (1 - a); o[3] = a + o[3] * (1 - a);
            }
    };
    if (shadowType != 0) {
        const double off = c.d("ShadowHeight") * W * 0.15, blur = c.d("ShadowBlur") * W * 0.04 + c.px(0.3);
        std::vector<float> sh((size_t)W * H, 0.f);
        const int ox = (int)std::lround(off * 0.7), oy = (int)std::lround(off * 0.7);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const int sx = x - ox, sy = y - oy;
                if (sx >= 0 && sy >= 0 && sx < W && sy < H) sh[(size_t)y * W + x] = alpha[(size_t)sy * W + sx];
            }
        gaussianBlurPlane(sh, W, H, blur, blur);
        // under the frame: where the frame is not opaque
        Buf under;
        under.resize(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                float* u = under.at(x, y);
                u[3] = sh[(size_t)y * W + x] * (float)clampd(c.d("ShadowAmount"), 0, 1);
            }
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                float* o = c.out.at(x, y);
                const float* u = under.at(x, y);
                const float k = u[3] * (1 - o[3]);
                o[3] += k;
            }
    }
    if (glowType != 0) {
        const Color gc = c.color("GlowColor");
        const double amt = clampd(c.d("GlowAmount"), 0, 1), rad = amt * W * 0.03 + c.px(0.5);
        std::vector<float> g = alpha;
        gaussianBlurPlane(g, W, H, rad, rad);
        std::vector<float> cover((size_t)W * H);
        for (size_t i = 0; i < cover.size(); ++i) cover[i] = glowType == 2 || glowType == 1 ? std::max(0.f, g[i] - alpha[i]) : std::max(0.f, alpha[i] - g[i]) * 2;
        over(cover, gc.r, gc.g, gc.b, 1.f);
    }
    // emboss: the alpha's slope lights the frame; LightLocation (x right, y up, z height; default (-3, 3, 3) is upper
    // left) gives the step across the pixels, and a lower light gives sharper lighting
    const double fg = c.d("ForegroundAmount");
    if (std::fabs(fg) > 0.001) {
        const double lx = c.d("LightIntensity") * 0.5 + 0.1;
        double Lx, Ly, Lz;
        c.xyz("LightLocation", Lx, Ly, Lz);
        const double mx = std::max(std::fabs(Lx), std::fabs(Ly));
        const double ox = mx > 1e-6 ? Lx / mx : -1.0, oy = mx > 1e-6 ? -Ly / mx : -1.0;      // (the frame's y runs down)
        const double heightGain = 3.0 / clampd(Lz, 0.3, 30.0);
        std::vector<float> sm = alpha;
        gaussianBlurPlane(sm, W, H, c.px(1.5 + c.d("EmbossRoundness") * 4), c.px(1.5 + c.d("EmbossRoundness") * 4));
        // the smoothed alpha at a point of the grid and a step away (a whole step: the value itself)
        auto at = [&](double fx, double fy) {
            fx = clampd(fx, 0, W - 1); fy = clampd(fy, 0, H - 1);
            const int x0 = (int)fx, y0 = (int)fy, x1 = std::min(W - 1, x0 + 1), y1 = std::min(H - 1, y0 + 1);
            const float tx = (float)(fx - x0), ty = (float)(fy - y0);
            const float a = sm[(size_t)y0 * W + x0] * (1 - tx) + sm[(size_t)y0 * W + x1] * tx, b = sm[(size_t)y1 * W + x0] * (1 - tx) + sm[(size_t)y1 * W + x1] * tx;
            return a * (1 - ty) + b * ty;
        };
        for (int y = 1; y < H - 1; ++y)
            for (int x = 1; x < W - 1; ++x) {
                const float slope = (at(x + ox, y + oy) - at(x - ox, y - oy)) * (float)(fg * lx * 3 * c.scale * heightGain);
                float* o = c.out.at(x, y);
                for (int k = 0; k < 3; ++k) o[k] = std::min(std::max(o[k] + slope * o[3], 0.f), o[3]);
            }
    }
}

}  // namespace ref2
