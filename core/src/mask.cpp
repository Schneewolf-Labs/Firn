#include "firn/mask.h"

#include <algorithm>
#include <array>
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
    return polygons(w, h, {pts}, antialias);
}

Mask polygons(int w, int h, const std::vector<std::vector<std::pair<float, float>>>& polys, bool antialias) {
    Mask m(w, h);
    float miny = 1e9f, maxy = -1e9f;
    size_t total = 0;
    for (const auto& pts : polys) for (const auto& p : pts) { miny = std::min(miny, p.second); maxy = std::max(maxy, p.second); ++total; }
    if (total < 3) return m;
    const int iy0 = std::max(0, static_cast<int>(std::floor(miny))), iy1 = std::min(h, static_cast<int>(std::ceil(maxy)) + 1);
    const int ss = antialias ? 4 : 1;
    std::vector<float> acc(w);
    std::vector<float> xs;
    for (int y = iy0; y < iy1; ++y) {
        std::fill(acc.begin(), acc.end(), 0.0f);
        for (int s = 0; s < ss; ++s) {
            const float sy = y + (s + 0.5f) / ss;
            xs.clear();
            for (const auto& pts : polys) {
                const size_t n = pts.size();
                if (n < 3) continue;
                for (size_t i = 0; i < n; ++i) {
                    const auto& a = pts[i];
                    const auto& b = pts[(i + 1) % n];
                    if ((a.second <= sy) == (b.second <= sy)) continue;  // no crossing
                    const float t = (sy - a.second) / (b.second - a.second);
                    xs.push_back(a.first + t * (b.first - a.first));
                }
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

Mask rounded_rectangle(int w, int h, float x0, float y0, float x1, float y1, float radius, bool antialias) {
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    const float r = std::clamp(radius, 0.0f, std::min(x1 - x0, y1 - y0) * 0.5f);
    if (r <= 0.0f) return rectangle(w, h, x0, y0, x1, y1, antialias);
    std::vector<std::pair<float, float>> pts;
    const int arc = std::max(4, static_cast<int>(r));  // segments per corner
    auto corner = [&](float cx, float cy, float a0) {
        for (int i = 0; i <= arc; ++i) {
            const float a = a0 + (3.14159265f * 0.5f) * i / arc;
            pts.emplace_back(cx + r * std::cos(a), cy + r * std::sin(a));
        }
    };
    corner(x1 - r, y0 + r, -3.14159265f * 0.5f);  // top-right
    corner(x1 - r, y1 - r, 0.0f);                  // bottom-right
    corner(x0 + r, y1 - r, 3.14159265f * 0.5f);   // bottom-left
    corner(x0 + r, y0 + r, 3.14159265f);          // top-left
    return polygon(w, h, pts, antialias);
}

Mask regular_polygon(int w, int h, float cx, float cy, float rx, float ry, int sides, float rotation_degrees, bool antialias) {
    sides = std::max(3, sides);
    std::vector<std::pair<float, float>> pts;
    const float rot = rotation_degrees * 3.14159265f / 180.0f - 3.14159265f * 0.5f;
    for (int i = 0; i < sides; ++i) {
        const float a = rot + 2.0f * 3.14159265f * i / sides;
        pts.emplace_back(cx + rx * std::cos(a), cy + ry * std::sin(a));
    }
    return polygon(w, h, pts, antialias);
}

Mask star(int w, int h, float cx, float cy, float rx, float ry, int points, float inner_ratio, float rotation_degrees, bool antialias) {
    points = std::max(3, points);
    inner_ratio = std::clamp(inner_ratio, 0.05f, 1.0f);
    std::vector<std::pair<float, float>> pts;
    const float rot = rotation_degrees * 3.14159265f / 180.0f - 3.14159265f * 0.5f;
    for (int i = 0; i < points * 2; ++i) {
        const float a = rot + 3.14159265f * i / points;
        const float k = (i % 2 == 0) ? 1.0f : inner_ratio;
        pts.emplace_back(cx + rx * k * std::cos(a), cy + ry * k * std::sin(a));
    }
    return polygon(w, h, pts, antialias);
}

Mask polyline(int w, int h, const std::vector<std::pair<float, float>>& pts, float width, bool antialias) {
    Mask m(w, h);
    if (pts.empty()) return m;
    const float r = std::max(width * 0.5f, 0.5f);
    // Coverage from distance to the nearest segment; 1px antialiasing band.
    float minx = pts[0].first, maxx = minx, miny = pts[0].second, maxy = miny;
    for (const auto& p : pts) { minx = std::min(minx, p.first); maxx = std::max(maxx, p.first); miny = std::min(miny, p.second); maxy = std::max(maxy, p.second); }
    const int x0 = std::max(0, static_cast<int>(std::floor(minx - r - 1))), x1 = std::min(w, static_cast<int>(std::ceil(maxx + r + 1)) + 1);
    const int y0 = std::max(0, static_cast<int>(std::floor(miny - r - 1))), y1 = std::min(h, static_cast<int>(std::ceil(maxy + r + 1)) + 1);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            float best = 1e9f;
            for (size_t i = 0; i < pts.size(); ++i) {
                const auto& a = pts[i];
                const auto& b = pts[std::min(i + 1, pts.size() - 1)];
                const float dx = b.first - a.first, dy = b.second - a.second;
                const float len2 = dx * dx + dy * dy;
                const float t = len2 > 0 ? std::clamp(((px - a.first) * dx + (py - a.second) * dy) / len2, 0.0f, 1.0f) : 0.0f;
                const float cx = a.first + dx * t, cy = a.second + dy * t;
                best = std::min(best, std::hypot(px - cx, py - cy));
            }
            const float cov = antialias ? std::clamp(r + 0.5f - best, 0.0f, 1.0f) : (best <= r ? 1.0f : 0.0f);
            if (cov > 0.0f) m.at(x, y) = to_u8(cov);
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


// --- Selections > Modify ----------------------------------------------------

void feather_inside(Mask& m, float radius) {
    Mask soft = m;
    feather(soft, radius);
    for (size_t i = 0; i < m.size(); ++i) m.data()[i] = std::min(m.data()[i], soft.data()[i]);
}

void feather_outside(Mask& m, float radius) {
    Mask soft = m;
    feather(soft, radius);
    for (size_t i = 0; i < m.size(); ++i) m.data()[i] = std::max(m.data()[i], soft.data()[i]);
}

void unfeather(Mask& m) {
    for (size_t i = 0; i < m.size(); ++i) m.data()[i] = m.data()[i] >= 128 ? 255 : 0;
}

void smooth(Mask& m, int amount, bool preserve_corners) {
    if (amount <= 0 || m.empty()) return;
    Mask soft = m;
    feather(soft, static_cast<float>(amount));
    // Re-threshold around the middle with a short ramp so the edge stays
    // anti-aliased; preserving corners keeps the original where the blur
    // only pulled pixels inward at convex corners (min/max with the source).
    for (size_t i = 0; i < m.size(); ++i) {
        const int v = soft.data()[i];
        const int band = 24;
        int out = v <= 128 - band ? 0 : v >= 128 + band ? 255 : (v - (128 - band)) * 255 / (2 * band);
        if (preserve_corners && m.data()[i] == 255 && v >= 96) out = 255;
        m.data()[i] = static_cast<uint8_t>(out);
    }
}

void shape_antialias(Mask& m, bool inside, bool outside) {
    Mask soft = m;
    feather(soft, 1.0f);
    for (size_t i = 0; i < m.size(); ++i) {
        const uint8_t a = m.data()[i], b = soft.data()[i];
        m.data()[i] = inside && outside ? b : inside ? std::min(a, b) : outside ? std::max(a, b) : a;
    }
}

void remove_specks_and_holes(Mask& m, int speck_size, int hole_size) {
    if (m.empty()) return;
    const int w = m.width(), h = m.height();
    std::vector<int> label(static_cast<size_t>(w) * h, -1);
    std::vector<int> area;
    std::vector<int> stack;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            if (label[i] >= 0) continue;
            const bool on = m.data()[i] >= 128;
            const int id = static_cast<int>(area.size());
            area.push_back(0);
            label[i] = id;
            stack.assign(1, static_cast<int>(i));
            while (!stack.empty()) {
                const int c = stack.back(); stack.pop_back();
                ++area[id];
                const int cx = c % w, cy = c / w;
                const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (const auto& d : nb) {
                    const int nx = cx + d[0], ny = cy + d[1];
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    const size_t ni = static_cast<size_t>(ny) * w + nx;
                    if (label[ni] >= 0 || (m.data()[ni] >= 128) != on) continue;
                    label[ni] = id;
                    stack.push_back(static_cast<int>(ni));
                }
            }
        }
    for (size_t i = 0; i < m.size(); ++i) {
        const bool on = m.data()[i] >= 128;
        const int a = area[label[i]];
        if (on && a <= speck_size) m.data()[i] = 0;
        else if (!on && a <= hole_size) m.data()[i] = 255;
    }
}

Mask select_color_range(const Image& img, Color color, int tolerance, int softness) {
    Mask out(img.width(), img.height(), 0);
    const uint8_t* p = img.data();
    for (size_t i = 0; i < out.size(); ++i) {
        const int d = std::max({std::abs(p[i * 4] - color.r), std::abs(p[i * 4 + 1] - color.g), std::abs(p[i * 4 + 2] - color.b)});
        int v = 0;
        if (d <= tolerance) v = 255;
        else if (softness > 0 && d < tolerance + softness) v = 255 - (d - tolerance) * 255 / softness;
        out.data()[i] = static_cast<uint8_t>(v);
    }
    return out;
}

Mask select_similar(const Image& img, const Mask& selection, int tolerance) {
    Mask out(img.width(), img.height(), 0);
    if (selection.empty() || selection.width() != img.width() || selection.height() != img.height()) return out;
    // Colors under the selection, quantized to 32 levels per channel, then
    // grown by the tolerance so the lookup is one table read per pixel.
    const int q = 32, step = 256 / q;
    std::vector<uint8_t> grid(static_cast<size_t>(q) * q * q, 0);
    const uint8_t* p = img.data();
    for (size_t i = 0; i < out.size(); ++i)
        if (selection.data()[i] >= 128) grid[(static_cast<size_t>(p[i * 4] / step) * q + p[i * 4 + 1] / step) * q + p[i * 4 + 2] / step] = 1;
    const int r = (tolerance + step - 1) / step;
    for (int axis = 0; axis < 3 && r > 0; ++axis) {
        std::vector<uint8_t> next(grid.size(), 0);
        for (int a = 0; a < q; ++a)
            for (int b = 0; b < q; ++b)
                for (int c = 0; c < q; ++c) {
                    int on = 0;
                    for (int k = -r; k <= r && !on; ++k) {
                        int aa = a, bb = b, cc = c;
                        (axis == 0 ? aa : axis == 1 ? bb : cc) += k;
                        if (aa < 0 || bb < 0 || cc < 0 || aa >= q || bb >= q || cc >= q) continue;
                        on = grid[(static_cast<size_t>(aa) * q + bb) * q + cc];
                    }
                    next[(static_cast<size_t>(a) * q + b) * q + c] = static_cast<uint8_t>(on);
                }
        grid.swap(next);
    }
    for (size_t i = 0; i < out.size(); ++i)
        if (grid[(static_cast<size_t>(p[i * 4] / step) * q + p[i * 4 + 1] / step) * q + p[i * 4 + 2] / step]) out.data()[i] = 255;
    return out;
}

// --- Edge helpers -----------------------------------------------------------

std::vector<float> edge_map(const Image& img) {
    const int w = img.width(), h = img.height();
    std::vector<float> luma(static_cast<size_t>(w) * h), out(static_cast<size_t>(w) * h, 0.0f);
    const uint8_t* p = img.data();
    for (size_t i = 0; i < luma.size(); ++i) luma[i] = (0.299f * p[i * 4] + 0.587f * p[i * 4 + 1] + 0.114f * p[i * 4 + 2]) / 255.0f;
    auto L = [&](int x, int y) { return luma[static_cast<size_t>(std::clamp(y, 0, h - 1)) * w + std::clamp(x, 0, w - 1)]; };
    float peak = 1e-6f;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float gx = (L(x + 1, y - 1) + 2 * L(x + 1, y) + L(x + 1, y + 1)) - (L(x - 1, y - 1) + 2 * L(x - 1, y) + L(x - 1, y + 1));
            const float gy = (L(x - 1, y + 1) + 2 * L(x, y + 1) + L(x + 1, y + 1)) - (L(x - 1, y - 1) + 2 * L(x, y - 1) + L(x + 1, y - 1));
            const float g = std::sqrt(gx * gx + gy * gy);
            out[static_cast<size_t>(y) * w + x] = g;
            peak = std::max(peak, g);
        }
    for (float& v : out) v /= peak;
    return out;
}

std::pair<float, float> seek_edge(const std::vector<float>& edges, int w, int h, float x, float y, int range) {
    const int cx = static_cast<int>(std::floor(x)), cy = static_cast<int>(std::floor(y));
    float best = -1.0f;
    std::pair<float, float> at{x, y};
    for (int dy = -range; dy <= range; ++dy)
        for (int dx = -range; dx <= range; ++dx) {
            const int px = cx + dx, py = cy + dy;
            if (px < 0 || py < 0 || px >= w || py >= h || dx * dx + dy * dy > range * range) continue;
            // Prefer strong edges, then nearer ones.
            const float score = edges[static_cast<size_t>(py) * w + px] - 0.002f * (dx * dx + dy * dy);
            if (score > best) { best = score; at = {px + 0.5f, py + 0.5f}; }
        }
    return best > 0.05f ? at : std::pair<float, float>{x, y};
}

std::vector<std::pair<float, float>> edge_path(const std::vector<float>& edges, int w, int h, std::pair<float, float> a, std::pair<float, float> b) {
    std::vector<std::pair<float, float>> out;
    const int ax = std::clamp(static_cast<int>(a.first), 0, w - 1), ay = std::clamp(static_cast<int>(a.second), 0, h - 1);
    const int bx = std::clamp(static_cast<int>(b.first), 0, w - 1), by = std::clamp(static_cast<int>(b.second), 0, h - 1);
    // Corridor: the segment's bounding box grown by a margin.
    const int margin = std::max(8, static_cast<int>(std::hypot(bx - ax, by - ay) * 0.5f));
    const int x0 = std::max(0, std::min(ax, bx) - margin), y0 = std::max(0, std::min(ay, by) - margin);
    const int x1 = std::min(w - 1, std::max(ax, bx) + margin), y1 = std::min(h - 1, std::max(ay, by) + margin);
    const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    const float inf = 1e30f;
    std::vector<float> dist(static_cast<size_t>(cw) * ch, inf);
    std::vector<int> prev(dist.size(), -1);
    std::vector<uint8_t> done(dist.size(), 0);
    auto idx = [&](int x, int y) { return static_cast<size_t>(y - y0) * cw + (x - x0); };
    // Simple binary heap over (cost, index).
    std::vector<std::pair<float, int>> heap;
    auto push = [&](float c, int i) { heap.emplace_back(c, i); std::push_heap(heap.begin(), heap.end(), [](const auto& l, const auto& r) { return l.first > r.first; }); };
    dist[idx(ax, ay)] = 0.0f;
    push(0.0f, static_cast<int>(idx(ax, ay)));
    const int target = static_cast<int>(idx(bx, by));
    while (!heap.empty()) {
        std::pop_heap(heap.begin(), heap.end(), [](const auto& l, const auto& r) { return l.first > r.first; });
        const auto [c, i] = heap.back(); heap.pop_back();
        if (done[i]) continue;
        done[i] = 1;
        if (i == target) break;
        const int cx = x0 + i % cw, cy = y0 + i / cw;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                const int nx = cx + dx, ny = cy + dy;
                if (nx < x0 || ny < y0 || nx > x1 || ny > y1) continue;
                const size_t ni = idx(nx, ny);
                if (done[ni]) continue;
                const float step = (dx && dy) ? 1.4142f : 1.0f;
                const float cost = c + step * (1.05f - edges[static_cast<size_t>(ny) * w + nx]);
                if (cost < dist[ni]) { dist[ni] = cost; prev[ni] = i; push(cost, static_cast<int>(ni)); }
            }
    }
    if (dist[target] >= inf) { out.push_back(a); out.push_back(b); return out; }
    for (int i = target; i >= 0; i = prev[i]) out.emplace_back(x0 + i % cw + 0.5f, y0 + i / cw + 0.5f);
    std::reverse(out.begin(), out.end());
    return out;
}

std::vector<std::pair<float, float>> smooth_polygon(const std::vector<std::pair<float, float>>& pts, int amount, bool closed) {
    const int n = static_cast<int>(pts.size());
    const int win = std::clamp(amount, 0, 100) / 10;
    if (win <= 0 || n < 3) return pts;
    std::vector<std::pair<float, float>> out(pts.size());
    for (int i = 0; i < n; ++i) {
        float sx = 0, sy = 0; int cnt = 0;
        for (int k = -win; k <= win; ++k) {
            int j = i + k;
            if (closed) j = (j % n + n) % n;
            else if (j < 0 || j >= n) continue;
            sx += pts[j].first; sy += pts[j].second; ++cnt;
        }
        out[i] = {sx / cnt, sy / cnt};
    }
    if (!closed) { out.front() = pts.front(); out.back() = pts.back(); }
    return out;
}

}  // namespace mask
}  // namespace firn

namespace firn::mask {

namespace {

// k-means color model: `k` centers fitted to the marked pixels (subsampled).
std::vector<std::array<float, 3>> color_model(const Image& img, const std::vector<uint8_t>& label, uint8_t want, int k) {
    std::vector<std::array<float, 3>> samples;
    size_t n = 0;
    for (size_t i = 0; i < label.size(); ++i) if (label[i] == want) ++n;
    const size_t stride = std::max<size_t>(1, n / 4000);
    size_t seen = 0;
    for (size_t i = 0; i < label.size(); ++i) {
        if (label[i] != want) continue;
        if (seen++ % stride) continue;
        const uint8_t* p = img.data() + i * 4;
        samples.push_back({p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f});
    }
    std::vector<std::array<float, 3>> centers;
    if (samples.empty()) return centers;
    k = std::min<int>(k, static_cast<int>(samples.size()));
    for (int c = 0; c < k; ++c) centers.push_back(samples[samples.size() * c / k]);
    std::vector<int> owner(samples.size(), 0);
    for (int iter = 0; iter < 10; ++iter) {
        for (size_t i = 0; i < samples.size(); ++i) {
            float best = 1e30f; int bi = 0;
            for (int c = 0; c < k; ++c) {
                float d = 0; for (int j = 0; j < 3; ++j) { const float t = samples[i][j] - centers[c][j]; d += t * t; }
                if (d < best) { best = d; bi = c; }
            }
            owner[i] = bi;
        }
        std::vector<std::array<float, 3>> sum(k, {0, 0, 0}); std::vector<int> cnt(k, 0);
        for (size_t i = 0; i < samples.size(); ++i) { for (int j = 0; j < 3; ++j) sum[owner[i]][j] += samples[i][j]; ++cnt[owner[i]]; }
        for (int c = 0; c < k; ++c) if (cnt[c]) for (int j = 0; j < 3; ++j) centers[c][j] = sum[c][j] / cnt[c];
    }
    return centers;
}

float model_distance(const std::vector<std::array<float, 3>>& centers, const uint8_t* p) {
    float best = 1e30f;
    const float c0 = p[0] / 255.0f, c1 = p[1] / 255.0f, c2 = p[2] / 255.0f;
    for (const auto& m : centers) {
        const float d = (c0 - m[0]) * (c0 - m[0]) + (c1 - m[1]) * (c1 - m[1]) + (c2 - m[2]) * (c2 - m[2]);
        best = std::min(best, d);
    }
    return best;
}

// Geodesic distance from the pixels with `seed` label over the likelihood
// map, by repeated forward/backward chamfer sweeps with 8 neighbors.
std::vector<float> geodesic(const std::vector<float>& like, const std::vector<uint8_t>& label, uint8_t seed, int w, int h) {
    std::vector<float> d(like.size(), 1e30f);
    for (size_t i = 0; i < d.size(); ++i) if (label[i] == seed) d[i] = 0.0f;
    const float eps = 0.5f / static_cast<float>(std::max(w, h));   // spatial tie-breaker
    auto relax = [&](int x, int y, int nx, int ny, float spatial) {
        if (nx < 0 || ny < 0 || nx >= w || ny >= h) return;
        const size_t i = static_cast<size_t>(y) * w + x, j = static_cast<size_t>(ny) * w + nx;
        const float c = d[j] + std::abs(like[i] - like[j]) + spatial;
        if (c < d[i]) d[i] = c;
    };
    const float diag = eps * 1.41421356f;
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                relax(x, y, x - 1, y, eps); relax(x, y, x, y - 1, eps);
                relax(x, y, x - 1, y - 1, diag); relax(x, y, x + 1, y - 1, diag);
            }
        for (int y = h - 1; y >= 0; --y)
            for (int x = w - 1; x >= 0; --x) {
                relax(x, y, x + 1, y, eps); relax(x, y, x, y + 1, eps);
                relax(x, y, x + 1, y + 1, diag); relax(x, y, x - 1, y + 1, diag);
            }
    }
    return d;
}

// Nearest-neighbor mask shrink keeping any mark in the block (thin scribbles survive).
Mask shrink_marks(const Mask& m, int sw, int sh) {
    Mask out(sw, sh, 0);
    if (m.empty()) return out;
    for (int y = 0; y < sh; ++y) {
        const int y0 = y * m.height() / sh, y1 = std::max(y0 + 1, (y + 1) * m.height() / sh);
        for (int x = 0; x < sw; ++x) {
            const int x0 = x * m.width() / sw, x1 = std::max(x0 + 1, (x + 1) * m.width() / sw);
            uint8_t v = 0;
            for (int yy = y0; yy < y1; ++yy) for (int xx = x0; xx < x1; ++xx) v = std::max(v, m.at(xx, yy));
            out.at(x, y) = v;
        }
    }
    return out;
}

Mask grow_mask(const Mask& m, int w, int h) {
    Mask out(w, h, 0);
    const float sx = static_cast<float>(m.width()) / w, sy = static_cast<float>(m.height()) / h;
    for (int y = 0; y < h; ++y) {
        const float fy = (y + 0.5f) * sy - 0.5f;
        const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, m.height() - 1), y1 = std::min(y0 + 1, m.height() - 1);
        const float ty = std::clamp(fy - y0, 0.0f, 1.0f);
        for (int x = 0; x < w; ++x) {
            const float fx = (x + 0.5f) * sx - 0.5f;
            const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, m.width() - 1), x1 = std::min(x0 + 1, m.width() - 1);
            const float tx = std::clamp(fx - x0, 0.0f, 1.0f);
            const float v = (m.at(x0, y0) * (1 - tx) + m.at(x1, y0) * tx) * (1 - ty) + (m.at(x0, y1) * (1 - tx) + m.at(x1, y1) * tx) * ty;
            out.at(x, y) = static_cast<uint8_t>(v + 0.5f);
        }
    }
    return out;
}

}  // namespace

Mask foreground_select(const Image& img, const Mask& fg, const Mask& bg, const Mask& region) {
    const int W = img.width(), H = img.height();
    if (W <= 0 || H <= 0) return {};
    // Solve at a working size of about 1.5 MP.
    const double budget = 1.5e6;
    const double scale = std::min(1.0, std::sqrt(budget / (static_cast<double>(W) * H)));
    const int w = std::max(1, static_cast<int>(W * scale)), h = std::max(1, static_cast<int>(H * scale));
    const bool scaled = w != W || h != H;
    const Image small = scaled ? raster::resample(img, w, h, raster::Filter::Bilinear) : img;
    const Mask sfg = scaled ? shrink_marks(fg, w, h) : fg;
    const Mask sbg = scaled ? shrink_marks(bg, w, h) : bg;
    const Mask sreg = region.empty() ? Mask() : scaled ? shrink_marks(region, w, h) : region;

    // Labels: 1 foreground, 2 background, 0 unknown.
    std::vector<uint8_t> label(static_cast<size_t>(w) * h, 0);
    bool any_fg = false, any_bg = false;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            if (!sbg.empty() && sbg.at(x, y)) { label[i] = 2; any_bg = true; }
            else if (!sreg.empty() && sreg.at(x, y) < 128) { label[i] = 2; any_bg = true; }
            if (!sfg.empty() && sfg.at(x, y)) { label[i] = 1; any_fg = true; }
        }
    if (!any_fg) return Mask(W, H, 0);
    if (!any_bg) {   // nothing says background: the outer edge does
        for (int x = 0; x < w; ++x) { if (!label[x]) label[x] = 2; if (!label[static_cast<size_t>(h - 1) * w + x]) label[static_cast<size_t>(h - 1) * w + x] = 2; }
        for (int y = 0; y < h; ++y) { if (!label[static_cast<size_t>(y) * w]) label[static_cast<size_t>(y) * w] = 2; if (!label[static_cast<size_t>(y) * w + w - 1]) label[static_cast<size_t>(y) * w + w - 1] = 2; }
    }
    // Color likelihood of foreground per pixel.
    const auto fg_model = color_model(small, label, 1, 8), bg_model = color_model(small, label, 2, 8);
    std::vector<float> like(label.size(), 0.5f);
    for (size_t i = 0; i < like.size(); ++i) {
        const uint8_t* p = small.data() + i * 4;
        const float df = model_distance(fg_model, p), db = model_distance(bg_model, p);
        like[i] = (db + 1e-4f) / (df + db + 2e-4f);
    }
    const std::vector<float> dfg = geodesic(like, label, 1, w, h), dbg = geodesic(like, label, 2, w, h);
    Mask out(w, h, 0);
    for (size_t i = 0; i < like.size(); ++i) {
        float a;
        if (label[i] == 1) a = 1.0f;
        else if (label[i] == 2) a = 0.0f;
        else {
            const float t = (dbg[i] + 1e-6f) / (dfg[i] + dbg[i] + 2e-6f);   // 1 = far from background, near foreground
            a = std::clamp((t - 0.5f) * 8.0f + 0.5f, 0.0f, 1.0f);
        }
        out.data()[i] = static_cast<uint8_t>(a * 255.0f + 0.5f);
    }
    // Drop stray islands and pinholes smaller than a sliver of the image.
    const int sliver = std::max(4, static_cast<int>(static_cast<double>(w) * h * 0.0005));
    remove_specks_and_holes(out, sliver, sliver);
    return scaled ? grow_mask(out, W, H) : out;
}

Mask warp(const Mask& m, const float H[9]) {
    if (m.empty()) return m;
    Image tmp(m.width(), m.height(), {255, 255, 255, 0});
    for (size_t i = 0; i < m.size(); ++i) tmp.data()[i * 4 + 3] = m.data()[i];
    const Image w = raster::warp(tmp, H, m.width(), m.height());
    Mask out(m.width(), m.height(), 0);
    for (size_t i = 0; i < out.size(); ++i) out.data()[i] = w.data()[i * 4 + 3];
    return out;
}

}  // namespace firn::mask
