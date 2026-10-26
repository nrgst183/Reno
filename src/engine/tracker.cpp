// Motion tracking: see tracker.h.
#include "tracker.h"
#include <algorithm>
#include <cmath>

namespace reno {

GrayImage toGray(const Image& img, int maxW, double* scale)
{
    GrayImage g;
    if (img.w < 1 || img.h < 1) return g;
    const double s = maxW > 0 && img.w > maxW ? (double)maxW / img.w : 1.0;
    g.w = std::max(1, (int)std::lround(img.w * s));
    g.h = std::max(1, (int)std::lround(img.h * s));
    g.px.assign((size_t)g.w * (size_t)g.h, 0.f);
    const bool fl = img.depth == PixelDepth::Float;
    for (int y = 0; y < g.h; ++y) {
        const int y0 = (int)std::floor(y / s), y1 = std::max(y0 + 1, (int)std::floor((y + 1) / s));
        for (int x = 0; x < g.w; ++x) {
            const int x0 = (int)std::floor(x / s), x1 = std::max(x0 + 1, (int)std::floor((x + 1) / s));
            double sum = 0;
            int n = 0;
            for (int yy = y0; yy < y1 && yy < img.h; ++yy) {
                const int row = img.h - 1 - yy;
                for (int xx = x0; xx < x1 && xx < img.w; ++xx) {
                    if (fl) {
                        const float* p = (const float*)img.row(row) + (size_t)xx * 4;
                        sum += 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
                    } else {
                        const uint8_t* p = img.row(row) + (size_t)xx * 4;
                        sum += (0.2126 * p[2] + 0.7152 * p[1] + 0.0722 * p[0]) / 255.0;
                    }
                    ++n;
                }
            }
            g.px[(size_t)y * (size_t)g.w + (size_t)x] = n ? (float)(sum / n) : 0.f;
        }
    }
    if (scale) *scale = (double)g.w / img.w;
    return g;
}

GrayImage halfSize(const GrayImage& g)
{
    GrayImage h;
    h.w = std::max(1, g.w / 2);
    h.h = std::max(1, g.h / 2);
    h.px.assign((size_t)h.w * (size_t)h.h, 0.f);
    for (int y = 0; y < h.h; ++y)
        for (int x = 0; x < h.w; ++x) {
            const int x0 = std::min(g.w - 1, 2 * x), x1 = std::min(g.w - 1, 2 * x + 1), y0 = std::min(g.h - 1, 2 * y), y1 = std::min(g.h - 1, 2 * y + 1);
            h.px[(size_t)y * (size_t)h.w + (size_t)x] = 0.25f * (g.at(x0, y0) + g.at(x1, y0) + g.at(x0, y1) + g.at(x1, y1));
        }
    return h;
}

static float sample(const GrayImage& g, double x, double y)
{
    x = std::min((double)g.w - 1.001, std::max(0.0, x));
    y = std::min((double)g.h - 1.001, std::max(0.0, y));
    const int x0 = (int)x, y0 = (int)y;
    const float fx = (float)(x - x0), fy = (float)(y - y0);
    const float a = g.at(x0, y0), b = g.at(x0 + 1, y0), c = g.at(x0, y0 + 1), d = g.at(x0 + 1, y0 + 1);
    return a * (1 - fx) * (1 - fy) + b * fx * (1 - fy) + c * (1 - fx) * fy + d * fx * fy;
}

// The patch of 'g' of tw x th centred on (cx, cy), with its mean taken off; its size in 'norm'.
static std::vector<float> patch(const GrayImage& g, double cx, double cy, int tw, int th, double& norm)
{
    std::vector<float> t((size_t)tw * (size_t)th);
    double mean = 0;
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x) {
            const float v = sample(g, cx - (tw - 1) / 2.0 + x, cy - (th - 1) / 2.0 + y);
            t[(size_t)y * (size_t)tw + (size_t)x] = v;
            mean += v;
        }
    mean /= (double)t.size();
    double ss = 0;
    for (float& v : t) {
        v = (float)(v - mean);
        ss += (double)v * v;
    }
    norm = std::sqrt(ss);
    return t;
}

double BoxTracker::ncc(const GrayImage& g, int x, int y, const std::vector<float>& t, int tw, int th, double tnorm)
{
    if (x < 0 || y < 0 || x + tw > g.w || y + th > g.h || tnorm < 1e-9) return -1.0;
    double sum = 0, sum2 = 0, cross = 0;
    for (int j = 0; j < th; ++j) {
        const float* row = &g.px[(size_t)(y + j) * (size_t)g.w + (size_t)x];
        const float* tr = &t[(size_t)j * (size_t)tw];
        for (int i = 0; i < tw; ++i) {
            const double v = row[i];
            sum += v;
            sum2 += v * v;
            cross += v * tr[i];
        }
    }
    const double n = (double)tw * th;
    const double var = sum2 - sum * sum / n;
    if (var < 1e-12) return 0.0;
    return cross / (std::sqrt(var) * tnorm);
}

void BoxTracker::setTemplate(const GrayImage& g, double cx, double cy)
{
    tmpl_ = patch(g, cx, cy, tw_, th_, tnorm_);
}

bool BoxTracker::start(const GrayImage& first, double cx, double cy, double w, double h)
{
    tw_ = std::max(5, (int)std::lround(w)) | 1;
    th_ = std::max(5, (int)std::lround(h)) | 1;
    if (first.w < tw_ || first.h < th_ || cx < 0 || cy < 0 || cx >= first.w || cy >= first.h) return false;
    cx_ = cx;
    cy_ = cy;
    vx_ = vy_ = 0;
    setTemplate(first, cx, cy);
    return tnorm_ > 1e-6;
}

bool BoxTracker::step(const GrayImage& next, double& cx, double& cy, double* score)
{
    if (tmpl_.empty() || next.w < tw_ || next.h < th_) return false;
    const int radius = (int)std::lround(searchRadius > 0 ? searchRadius : std::max(12.0, 0.6 * std::max(tw_, th_)));
    const int hx = (tw_ - 1) / 2, hy = (th_ - 1) / 2;
    // where it is looked for: where it was going
    const double px = cx_ + vx_, py = cy_ + vy_;
    // further from there, a place must match a little better: a subject does not jump about.
    auto penalty = [&](double dx, double dy) { return 0.15 * (dx * dx + dy * dy) / ((double)radius * radius); };
    // coarse: at half size, the template halved too
    int bx = (int)std::lround(px) - hx, by = (int)std::lround(py) - hy;        // top-left of the best place so far
    if (tw_ >= 11 && th_ >= 11 && radius >= 8) {
        const GrayImage half = halfSize(next);
        const int htw = (tw_ / 2) | 1, hth = (th_ / 2) | 1;
        std::vector<float> ht((size_t)htw * (size_t)hth);
        double hmean = 0;
        for (int y = 0; y < hth; ++y)
            for (int x = 0; x < htw; ++x) {
                const double sx = hx + (x - (htw - 1) / 2.0) * 2.0, sy = hy + (y - (hth - 1) / 2.0) * 2.0;
                const int ix = std::min(tw_ - 1, std::max(0, (int)std::lround(sx))), iy = std::min(th_ - 1, std::max(0, (int)std::lround(sy)));
                const float v = tmpl_[(size_t)iy * (size_t)tw_ + (size_t)ix];
                ht[(size_t)y * (size_t)htw + (size_t)x] = v;
                hmean += v;
            }
        hmean /= (double)ht.size();
        double hss = 0;
        for (float& v : ht) {
            v = (float)(v - hmean);
            hss += (double)v * v;
        }
        const double hnorm = std::sqrt(hss);
        const int hr = radius / 2 + 1;
        const int hcx = (int)std::lround((px - 0.5) / 2.0) - (htw - 1) / 2, hcy = (int)std::lround((py - 0.5) / 2.0) - (hth - 1) / 2;
        double best = -2;
        int hbx = hcx, hby = hcy;
        for (int dy = -hr; dy <= hr; ++dy)
            for (int dx = -hr; dx <= hr; ++dx) {
                const double s = ncc(half, hcx + dx, hcy + dy, ht, htw, hth, hnorm) - penalty(2.0 * dx, 2.0 * dy);
                if (s > best) { best = s; hbx = hcx + dx; hby = hcy + dy; }
            }
        if (best > 0) {
            // the coarse centre, at full size
            const double ccx = (hbx + (htw - 1) / 2.0) * 2.0 + 0.5, ccy = (hby + (hth - 1) / 2.0) * 2.0 + 0.5;
            bx = (int)std::lround(ccx) - hx;
            by = (int)std::lround(ccy) - hy;
        }
    }
    // fine: at full size, a few pixels round the coarse place (or the whole radius for small boxes)
    const int fr = (tw_ >= 11 && th_ >= 11 && radius >= 8) ? 3 : radius;
    double best = -2, bestRaw = -2;
    int fx = bx, fy = by;
    for (int dy = -fr; dy <= fr; ++dy)
        for (int dx = -fr; dx <= fr; ++dx) {
            const double raw = ncc(next, bx + dx, by + dy, tmpl_, tw_, th_, tnorm_);
            const double s = raw - penalty(bx + dx + hx - px, by + dy + hy - py);
            if (s > best) { best = s; bestRaw = raw; fx = bx + dx; fy = by + dy; }
        }
    best = bestRaw;
    if (score) *score = best;
    if (best < minScore) return false;
    // between pixels: a parabola through the best and its neighbours, each way
    auto refine = [&](double l, double c, double r) {
        const double den = l - 2 * c + r;
        return std::fabs(den) > 1e-12 ? std::min(0.5, std::max(-0.5, 0.5 * (l - r) / den)) : 0.0;
    };
    const double sx = refine(ncc(next, fx - 1, fy, tmpl_, tw_, th_, tnorm_), best, ncc(next, fx + 1, fy, tmpl_, tw_, th_, tnorm_));
    const double sy = refine(ncc(next, fx, fy - 1, tmpl_, tw_, th_, tnorm_), best, ncc(next, fx, fy + 1, tmpl_, tw_, th_, tnorm_));
    vx_ = fx + hx + sx - cx_;
    vy_ = fy + hy + sy - cy_;
    cx_ = fx + hx + sx;
    cy_ = fy + hy + sy;
    cx = cx_;
    cy = cy_;
    // the template follows slowly when the match is good (a subject that turns or changes light a little)
    if (best > 0.8) {
        double n2 = 0;
        const std::vector<float> now = patch(next, cx_, cy_, tw_, th_, n2);
        double ss = 0;
        for (size_t i = 0; i < tmpl_.size(); ++i) {
            tmpl_[i] = 0.9f * tmpl_[i] + 0.1f * now[i];
            ss += (double)tmpl_[i] * tmpl_[i];
        }
        tnorm_ = std::sqrt(ss);
    }
    return true;
}

}  // namespace reno
