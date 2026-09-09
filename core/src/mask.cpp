#include "firn/mask.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace firn {

Mask::Mask(int width, int height, uint8_t fill)
    : width_(width), height_(height), data_(static_cast<size_t>(width) * height, fill) {}

raster::Rect Mask::bounds() const {
    raster::Rect r{width_, height_, 0, 0};
    for (int y = 0; y < height_; ++y) {
        const uint8_t* row = data_.data() + static_cast<size_t>(y) * width_;
        int x0 = -1, x1 = -1;
        for (int x = 0; x < width_; ++x) {
            if (row[x]) { if (x0 < 0) x0 = x; x1 = x; }
        }
        if (x0 < 0) continue;
        r.x0 = std::min(r.x0, x0);
        r.x1 = std::max(r.x1, x1 + 1);
        r.y0 = std::min(r.y0, y);
        r.y1 = y + 1;
    }
    return r.empty() ? raster::Rect{} : r;
}

namespace mask {

namespace {

uint8_t to_u8(float cov) { return static_cast<uint8_t>(std::clamp(cov, 0.0f, 1.0f) * 255.0f + 0.5f); }

// Overlap of [a0,a1) with the unit interval [i, i+1).
float overlap(float a0, float a1, int i) {
    return std::max(0.0f, std::min(a1, i + 1.0f) - std::max(a0, static_cast<float>(i)));
}

}  // namespace

Mask rectangle(int w, int h, float x0, float y0, float x1, float y1, bool antialias) {
    Mask m(w, h);
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    if (!antialias) { x0 = std::round(x0); x1 = std::round(x1); y0 = std::round(y0); y1 = std::round(y1); }
    const int ix0 = std::max(0, static_cast<int>(std::floor(x0))), ix1 = std::min(w, static_cast<int>(std::ceil(x1)));
    const int iy0 = std::max(0, static_cast<int>(std::floor(y0))), iy1 = std::min(h, static_cast<int>(std::ceil(y1)));
    for (int y = iy0; y < iy1; ++y) {
        const float cy = overlap(y0, y1, y);
        for (int x = ix0; x < ix1; ++x) m.at(x, y) = to_u8(cy * overlap(x0, x1, x));
    }
    return m;
}

Mask ellipse(int w, int h, float cx, float cy, float rx, float ry, bool antialias) {
    Mask m(w, h);
    rx = std::abs(rx); ry = std::abs(ry);
    if (rx <= 0.0f || ry <= 0.0f) return m;
    const int ix0 = std::max(0, static_cast<int>(std::floor(cx - rx)) - 1), ix1 = std::min(w, static_cast<int>(std::ceil(cx + rx)) + 1);
    const int iy0 = std::max(0, static_cast<int>(std::floor(cy - ry)) - 1), iy1 = std::min(h, static_cast<int>(std::ceil(cy + ry)) + 1);
    const int ss = antialias ? 4 : 1;  // supersample grid per axis
    for (int y = iy0; y < iy1; ++y) {
        for (int x = ix0; x < ix1; ++x) {
            int inside = 0;
            for (int sy = 0; sy < ss; ++sy) {
                for (int sx = 0; sx < ss; ++sx) {
                    const float px = x + (sx + 0.5f) / ss, py = y + (sy + 0.5f) / ss;
                    const float dx = (px - cx) / rx, dy = (py - cy) / ry;
                    if (dx * dx + dy * dy <= 1.0f) ++inside;
                }
            }
            m.at(x, y) = to_u8(static_cast<float>(inside) / (ss * ss));
        }
    }
    return m;
}

// Scanline polygon fill, even-odd rule, with vertical supersampling and
// exact horizontal span coverage.
Mask polygon(int w, int h, const std::vector<std::pair<float, float>>& pts, bool antialias) {
    Mask m(w, h);
    const size_t n = pts.size();
    if (n < 3) return m;
    float miny = pts[0].second, maxy = pts[0].second;
    for (const auto& p : pts) { miny = std::min(miny, p.second); maxy = std::max(maxy, p.second); }
    const int iy0 = std::max(0, static_cast<int>(std::floor(miny))), iy1 = std::min(h, static_cast<int>(std::ceil(maxy)) + 1);
    const int ss = antialias ? 4 : 1;
    std::vector<float> acc(w);
    std::vector<float> xs;
    for (int y = iy0; y < iy1; ++y) {
        std::fill(acc.begin(), acc.end(), 0.0f);
        for (int s = 0; s < ss; ++s) {
            const float sy = y + (s + 0.5f) / ss;
            xs.clear();
            for (size_t i = 0; i < n; ++i) {
                const auto& a = pts[i];
                const auto& b = pts[(i + 1) % n];
                if ((a.second <= sy) == (b.second <= sy)) continue;  // no crossing
                const float t = (sy - a.second) / (b.second - a.second);
                xs.push_back(a.first + t * (b.first - a.first));
            }
            std::sort(xs.begin(), xs.end());
            for (size_t i = 0; i + 1 < xs.size(); i += 2) {
                float x0 = xs[i], x1 = xs[i + 1];
                if (!antialias) { x0 = std::round(x0); x1 = std::round(x1); }
                const int a0 = std::max(0, static_cast<int>(std::floor(x0))), a1 = std::min(w, static_cast<int>(std::ceil(x1)));
                for (int x = a0; x < a1; ++x) acc[x] += overlap(x0, x1, x);
            }
        }
        for (int x = 0; x < w; ++x)
            if (acc[x] > 0.0f) m.at(x, y) = to_u8(acc[x] / ss);
    }
    return m;
}

Mask magic_wand(const Image& img, int x, int y, int tolerance, bool contiguous) {
    const int w = img.width(), h = img.height();
    Mask m(w, h);
    if (x < 0 || y < 0 || x >= w || y >= h) return m;
    const Color seed = img.get(x, y);
    auto matches = [&](int px, int py) {
        const Color c = img.get(px, py);
        return std::max({std::abs(c.r - seed.r), std::abs(c.g - seed.g), std::abs(c.b - seed.b),
                         std::abs(c.a - seed.a)}) <= tolerance;
    };
    if (!contiguous) {
        for (int py = 0; py < h; ++py)
            for (int px = 0; px < w; ++px)
                if (matches(px, py)) m.at(px, py) = 255;
        return m;
    }
    std::vector<std::pair<int, int>> stack{{x, y}};
    m.at(x, y) = 255;
    while (!stack.empty()) {
        auto [px, py] = stack.back();
        stack.pop_back();
        int lx = px;
        while (lx > 0 && !m.at(lx - 1, py) && matches(lx - 1, py)) --lx;
        int rx = px;
        while (rx + 1 < w && !m.at(rx + 1, py) && matches(rx + 1, py)) ++rx;
        for (int i = lx; i <= rx; ++i) m.at(i, py) = 255;
        for (int ny : {py - 1, py + 1}) {
            if (ny < 0 || ny >= h) continue;
            bool run = false;
            for (int i = lx; i <= rx; ++i) {
                if (!m.at(i, ny) && matches(i, ny)) {
                    if (!run) { stack.emplace_back(i, ny); run = true; }
                } else {
                    run = false;
                }
            }
        }
    }
    return m;
}

void combine(Mask& dst, const Mask& src, Combine mode) {
    if (mode == Combine::Replace || dst.empty()) { dst = src; return; }
    const size_t n = dst.size();
    uint8_t* d = dst.data();
    const uint8_t* s = src.data();
    switch (mode) {
        case Combine::Add:       for (size_t i = 0; i < n; ++i) d[i] = std::max(d[i], s[i]); break;
        case Combine::Subtract:  for (size_t i = 0; i < n; ++i) d[i] = static_cast<uint8_t>(std::max(0, d[i] - s[i])); break;
        case Combine::Intersect: for (size_t i = 0; i < n; ++i) d[i] = std::min(d[i], s[i]); break;
        case Combine::Replace: break;
    }
}

void invert(Mask& m) {
    uint8_t* d = m.data();
    for (size_t i = 0; i < m.size(); ++i) d[i] = 255 - d[i];
}

void feather(Mask& m, float radius) {
    const int w = m.width(), h = m.height();
    if (radius <= 0.0f || m.empty()) return;
    const float sigma = radius * 0.5f;
    const int extent = std::max(1, static_cast<int>(std::ceil(sigma * 3.0f)));
    std::vector<float> kernel(2 * extent + 1);
    float sum = 0.0f;
    for (int i = -extent; i <= extent; ++i) { kernel[i + extent] = std::exp(-(i * i) / (2.0f * sigma * sigma)); sum += kernel[i + extent]; }
    for (float& k : kernel) k /= sum;
    std::vector<float> tmp(m.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float a = 0.0f;
            for (int k = -extent; k <= extent; ++k) a += m.at(std::clamp(x + k, 0, w - 1), y) * kernel[k + extent];
            tmp[static_cast<size_t>(y) * w + x] = a;
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float a = 0.0f;
            for (int k = -extent; k <= extent; ++k) a += tmp[static_cast<size_t>(std::clamp(y + k, 0, h - 1)) * w + x] * kernel[k + extent];
            m.at(x, y) = static_cast<uint8_t>(std::clamp(a, 0.0f, 255.0f) + 0.5f);
        }
}

namespace {
// Circular max/min filter. O(r^2) per pixel; fine for the sizes selections use.
void morph(Mask& m, int r, bool grow) {
    const int w = m.width(), h = m.height();
    if (r <= 0 || m.empty()) return;
    Mask src = m;
    std::vector<int> half(r + 1);  // horizontal half-width of the disc at each dy
    for (int dy = 0; dy <= r; ++dy) half[dy] = static_cast<int>(std::floor(std::sqrt(static_cast<float>(r * r - dy * dy))));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t v = src.at(x, y);
            if (grow ? v == 255 : v == 0) continue;
            for (int dy = -r; dy <= r && (grow ? v < 255 : v > 0); ++dy) {
                const int yy = y + dy;
                if (yy < 0 || yy >= h) { if (!grow) v = 0; continue; }  // outside the image counts as unselected
                const int hw = half[std::abs(dy)];
                const int x0 = std::max(0, x - hw), x1 = std::min(w - 1, x + hw);
                if (!grow && (x - hw < 0 || x + hw > w - 1)) { v = 0; break; }
                const uint8_t* row = src.data() + static_cast<size_t>(yy) * w;
                for (int xx = x0; xx <= x1; ++xx) v = grow ? std::max(v, row[xx]) : std::min(v, row[xx]);
            }
            m.at(x, y) = v;
        }
    }
}
}  // namespace

void expand(Mask& m, int pixels) { morph(m, pixels, true); }
void contract(Mask& m, int pixels) { morph(m, pixels, false); }

}  // namespace mask
}  // namespace firn
