#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "effects_util.h"
#include "firn/effects.h"
#include "firn/raster.h"

namespace firn::effects {
using detail::clamp8;
using detail::remap;
using detail::remap_edges;
using detail::EdgeMode;

namespace {
EdgeMode em(Edge e) { return EdgeMode{e.mode, e.fill}; }
constexpr float kPi = 3.14159265358979f;
}  // namespace

void curlicues(Image& img, int columns, int rows, int radius, int strength) {
    columns = std::max(1, columns); rows = std::max(1, rows);
    const float cw = static_cast<float>(img.width()) / columns, ch = static_cast<float>(img.height()) / rows;
    const float r = std::max(1.0f, std::min(cw, ch) * 0.5f * std::clamp(radius, 1, 100) / 100.0f * 2.0f);
    const float k = std::clamp(strength, -100, 100) / 100.0f * 2.0f * kPi;
    remap(img, [&](float x, float y, float& sx, float& sy) {
        const float cx = (std::floor(x / cw) + 0.5f) * cw, cy = (std::floor(y / ch) + 0.5f) * ch;
        const float dx = x - cx, dy = y - cy;
        const float d = std::hypot(dx, dy);
        if (d >= r) { sx = x; sy = y; return; }
        const float a = k * (1.0f - d / r) * (1.0f - d / r);
        const float c = std::cos(a), s = std::sin(a);
        sx = cx + dx * c - dy * s;
        sy = cy + dx * s + dy * c;
    });
}

void displacement_map(Image& img, const Image& map, float intensity, bool two_d, float blur, Edge edge) {
    if (map.empty()) return;
    Image m = raster::resample(map, img.width(), img.height(), raster::Filter::Bilinear);
    if (blur > 0.0f) raster::gaussian_blur(m, blur);
    const float amount = intensity / 100.0f * std::min(img.width(), img.height());
    remap_edges(img, em(edge), [&](float x, float y, float& sx, float& sy) {
        const int px = std::clamp(static_cast<int>(x), 0, m.width() - 1), py = std::clamp(static_cast<int>(y), 0, m.height() - 1);
        const uint8_t* p = m.data() + (static_cast<size_t>(py) * m.width() + px) * 4;
        if (two_d) { sx = x + (p[0] / 255.0f - 0.5f) * amount; sy = y + (p[1] / 255.0f - 0.5f) * amount; }
        else { const float l = detail::luma_of(p) / 255.0f - 0.5f; sx = x + l * amount; sy = y + l * amount; }
    });
}

void polar_coordinates(Image& img, bool rect_to_polar, Edge edge) {
    const int w = img.width(), h = img.height();
    const float cx = w * 0.5f, cy = h * 0.5f, rmax = std::hypot(cx, cy);
    remap_edges(img, em(edge), [&](float x, float y, float& sx, float& sy) {
        if (rect_to_polar) {
            // Destination polar: angle from x, radius from y; source is rectangular.
            const float dx = x - cx, dy = y - cy;
            const float ang = std::atan2(dy, dx) + kPi;          // 0..2pi
            const float r = std::hypot(dx, dy) / rmax;
            sx = ang / (2.0f * kPi) * w;
            sy = r * h;
        } else {
            const float ang = x / w * 2.0f * kPi - kPi;
            const float r = y / h * rmax;
            sx = cx + std::cos(ang) * r;
            sy = cy + std::sin(ang) * r;
        }
    });
}

void spiky_halo(Image& img, float radius_percent, int spikes, float offset_percent, int bend) {
    const float cx = img.width() * 0.5f, cy = img.height() * 0.5f;
    const float base = std::min(cx, cy) * std::clamp(radius_percent, 1.0f, 200.0f) / 100.0f;
    const float amp = std::min(cx, cy) * std::clamp(offset_percent, 0.0f, 100.0f) / 100.0f;
    const float k = std::clamp(bend, -100, 100) / 100.0f * kPi;
    remap(img, [&](float x, float y, float& sx, float& sy) {
        const float dx = x - cx, dy = y - cy;
        const float d = std::hypot(dx, dy);
        if (d < 1e-3f) { sx = x; sy = y; return; }
        const float ang = std::atan2(dy, dx);
        const float halo = base + amp * std::sin(ang * spikes + k * d / base);
        const float t = std::clamp(1.0f - std::abs(d - halo) / std::max(amp, 8.0f), 0.0f, 1.0f);
        const float pull = 1.0f - 0.5f * t * t;
        sx = cx + dx * pull;
        sy = cy + dy * pull;
    });
}

void warp(Image& img, float cx_percent, float cy_percent, float size_percent, int strength) {
    const float cx = img.width() * cx_percent / 100.0f, cy = img.height() * cy_percent / 100.0f;
    const float radius = std::max(2.0f, std::min(img.width(), img.height()) * size_percent / 100.0f);
    const float s = std::clamp(strength, -100, 100) / 100.0f;
    remap(img, [&](float x, float y, float& sx, float& sy) {
        const float dx = x - cx, dy = y - cy;
        const float d = std::hypot(dx, dy);
        if (d >= radius || d == 0.0f) { sx = x; sy = y; return; }
        const float t = d / radius;
        const float f = s > 0 ? std::pow(t, 1.0f + s * 1.5f) : std::pow(t, 1.0f / (1.0f + (-s) * 1.5f));
        sx = cx + dx * f / t;
        sy = cy + dy * f / t;
    });
}

void wind(Image& img, bool from_left, int strength) {
    const int w = img.width(), h = img.height();
    const int len = std::max(1, w * std::clamp(strength, 1, 100) / 400);
    const Image src = img;
    uint8_t* d = img.data();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float acc[3] = {0, 0, 0}, wsum = 0;
            for (int i = 0; i < len; ++i) {
                const int sx = from_left ? x - i : x + i;
                if (sx < 0 || sx >= w) break;
                const float wt = std::exp(-3.0f * i / len);
                const uint8_t* s = src.data() + (static_cast<size_t>(y) * w + sx) * 4;
                const float l = detail::luma_of(s) / 255.0f;
                const float ww = wt * (0.3f + 0.7f * l);   // bright pixels streak further
                for (int c = 0; c < 3; ++c) acc[c] += s[c] * ww;
                wsum += ww;
            }
            uint8_t* p = d + (static_cast<size_t>(y) * w + x) * 4;
            if (wsum > 0) for (int c = 0; c < 3; ++c) p[c] = clamp8(std::max(static_cast<float>(p[c]), acc[c] / wsum));
        }
}

void circle(Image& img, Edge edge) {
    const int w = img.width(), h = img.height();
    const float cx = w * 0.5f, cy = h * 0.5f;
    remap_edges(img, em(edge), [&](float x, float y, float& sx, float& sy) {
        // Destination disc -> source rectangle: scale the radius by the
        // rectangle's boundary distance in that direction.
        const float dx = (x - cx) / cx, dy = (y - cy) / cy;
        const float d = std::hypot(dx, dy);
        if (d < 1e-6f) { sx = x; sy = y; return; }
        const float box = std::max(std::abs(dx), std::abs(dy)) / d;   // where the unit box boundary lies along this ray
        sx = cx + dx * cx * (1.0f / box);
        sy = cy + dy * cy * (1.0f / box);
        if (d > 1.0f) { sx = -1e6f; sy = -1e6f; }
    });
}

void cylinder(Image& img, bool vertical, int strength) {
    const int w = img.width(), h = img.height();
    const float s = std::clamp(strength, 0, 100) / 100.0f;
    remap(img, [&](float x, float y, float& sx, float& sy) {
        if (vertical) {
            const float t = (x / w) * 2.0f - 1.0f;
            const float u = std::asin(std::clamp(t, -1.0f, 1.0f)) / (kPi / 2);
            sx = ((t + (u - t) * s) * 0.5f + 0.5f) * w; sy = y;
        } else {
            const float t = (y / h) * 2.0f - 1.0f;
            const float u = std::asin(std::clamp(t, -1.0f, 1.0f)) / (kPi / 2);
            sy = ((t + (u - t) * s) * 0.5f + 0.5f) * h; sx = x;
        }
    });
}

void pentagon(Image& img, Edge edge) {
    const int w = img.width(), h = img.height();
    const float cx = w * 0.5f, cy = h * 0.5f;
    // Pentagon boundary radius as a function of angle (apothem 1).
    auto poly_r = [](float ang) { const float sector = 2.0f * kPi / 5; const float a = std::fmod(ang + kPi / 2 + 10 * kPi, sector) - sector / 2; return 1.0f / std::cos(a); };
    remap_edges(img, em(edge), [&](float x, float y, float& sx, float& sy) {
        const float dx = (x - cx) / cx, dy = (y - cy) / cy;
        const float d = std::hypot(dx, dy);
        if (d < 1e-6f) { sx = x; sy = y; return; }
        const float ang = std::atan2(dy, dx);
        const float pr = poly_r(ang) * 0.85f;   // inscribed size
        if (d > pr) { sx = -1e6f; sy = -1e6f; return; }
        const float box = std::max(std::abs(dx), std::abs(dy)) / d;
        const float k = (1.0f / box) / pr;
        sx = cx + dx * cx * k;
        sy = cy + dy * cy * k;
    });
}

void perspective(Image& img, bool vertical, int distortion, Edge edge) {
    const int w = img.width(), h = img.height();
    const float s = std::clamp(distortion, -100, 100) / 100.0f;
    remap_edges(img, em(edge), [&](float x, float y, float& sx, float& sy) {
        if (!vertical) {
            // Top edge narrows (s > 0) or bottom edge narrows (s < 0).
            const float t = y / h;
            const float scale = 1.0f - std::abs(s) * (s > 0 ? (1.0f - t) : t) * 0.9f;
            sx = (x - w * 0.5f) / scale + w * 0.5f; sy = y;
        } else {
            const float t = x / w;
            const float scale = 1.0f - std::abs(s) * (s > 0 ? (1.0f - t) : t) * 0.9f;
            sy = (y - h * 0.5f) / scale + h * 0.5f; sx = x;
        }
    });
}

void skew(Image& img, bool vertical, int angle, Edge edge) {
    const int w = img.width(), h = img.height();
    const float k = std::tan(std::clamp(angle, -45, 45) * kPi / 180.0f);
    remap_edges(img, em(edge), [&](float x, float y, float& sx, float& sy) {
        if (!vertical) { sx = x + (y - h * 0.5f) * k; sy = y; }
        else { sy = y + (x - w * 0.5f) * k; sx = x; }
    });
}

void feedback(Image& img, int opacity, int intensity, float cx_percent, float cy_percent, bool elliptical) {
    const int w = img.width(), h = img.height();
    const float cx = w * cx_percent / 100.0f, cy = h * cy_percent / 100.0f;
    const int count = std::clamp(intensity, 1, 20);
    const float op = std::clamp(opacity, 1, 100) / 100.0f;
    const Image base = img;
    for (int i = 1; i <= count; ++i) {
        const float scale = std::pow(0.8f, static_cast<float>(i));
        Image copy = base;
        remap(copy, [&](float x, float y, float& sx, float& sy) { sx = cx + (x - cx) / scale; sy = cy + (y - cy) / scale; });
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const float dx = (x - cx) / (w * 0.5f * scale), dy = (y - cy) / (h * 0.5f * scale);
                const bool inside = elliptical ? dx * dx + dy * dy <= 1.0f : std::abs(dx) <= 1.0f && std::abs(dy) <= 1.0f;
                if (!inside) continue;
                raster::blend_over(img, x, y, copy.get(x, y), op);
            }
    }
}

void rotating_mirror(Image& img, float angle, float cx_percent, float cy_percent, Edge edge) {
    const int w = img.width(), h = img.height();
    const float cx = w * cx_percent / 100.0f, cy = h * cy_percent / 100.0f;
    const float rad = angle * kPi / 180.0f, nx = std::cos(rad), ny = std::sin(rad);   // mirror line direction
    remap_edges(img, em(edge), [&](float x, float y, float& sx, float& sy) {
        const float dx = x - cx, dy = y - cy;
        const float side = dx * (-ny) + dy * nx;     // signed distance across the line
        if (side <= 0) { sx = x; sy = y; return; }
        sx = x - 2.0f * side * (-ny);
        sy = y - 2.0f * side * nx;
    });
}

void pattern(Image& img, float angle, float cx_percent, float cy_percent, float scale_percent, int rotation) {
    const int w = img.width(), h = img.height();
    const float cx = w * cx_percent / 100.0f, cy = h * cy_percent / 100.0f;
    const float cell = std::max(4.0f, std::min(w, h) * std::clamp(scale_percent, 1.0f, 100.0f) / 100.0f);
    const float rad = angle * kPi / 180.0f, c = std::cos(rad), s = std::sin(rad);
    const float rrad = rotation * kPi / 180.0f, rc = std::cos(rrad), rs = std::sin(rrad);
    remap(img, [&](float x, float y, float& sx, float& sy) {
        // Rotate into the tiling frame, mirror-tile, rotate the sample into the source.
        const float dx = x - cx, dy = y - cy;
        float u = dx * rc + dy * rs, v = -dx * rs + dy * rc;
        auto fold = [&](float t) { t = std::fmod(std::abs(t), 2.0f * cell); return t > cell ? 2.0f * cell - t : t; };
        u = fold(u); v = fold(v);
        sx = cx + u * c - v * s;
        sy = cy + u * s + v * c;
    });
}

void offset(Image& img, int dx, int dy, Edge edge) {
    remap_edges(img, em(edge), [&](float x, float y, float& sx, float& sy) { sx = x - dx; sy = y - dy; });
}

void seamless_tiling(Image& img, int method, int direction, int transition) {
    const int w = img.width(), h = img.height();
    const float t = std::clamp(transition, 0, 100) / 100.0f;
    const Image src = img;
    const bool horiz = direction != 2, vert = direction != 1;
    const int bw = std::max(1, static_cast<int>(w * 0.5f * t)), bh = std::max(1, static_cast<int>(h * 0.5f * t));
    uint8_t* d = img.data();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            // Weight towards the wrapped/mirrored counterpart near the edges (or corners for method 1).
            float fx = 0, fy = 0;
            if (horiz) { const int ex = std::min(x, w - 1 - x); if (ex < bw) fx = 0.5f * (1.0f - static_cast<float>(ex) / bw); }
            if (vert) { const int ey = std::min(y, h - 1 - y); if (ey < bh) fy = 0.5f * (1.0f - static_cast<float>(ey) / bh); }
            float f = method == 1 ? fx * fy * 2.0f : std::max(fx, fy);
            if (f <= 0.0f) continue;
            int ox = x, oy = y;
            if (method == 2) { if (fx > 0) ox = w - 1 - x; if (fy > 0) oy = h - 1 - y; }
            else { if (fx > 0) ox = (x + w / 2) % w; if (fy > 0) oy = (y + h / 2) % h; }
            const uint8_t* s = src.data() + (static_cast<size_t>(oy) * w + ox) * 4;
            uint8_t* p = d + (static_cast<size_t>(y) * w + x) * 4;
            for (int c = 0; c < 3; ++c) p[c] = clamp8(p[c] + (s[c] - p[c]) * f);
        }
}

void page_curl(Image& img, int corner, float width_percent, float height_percent, int radius, Color back, Color fill, bool transparent_fill) {
    const int w = img.width(), h = img.height();
    const float cw = w * std::clamp(width_percent, 1.0f, 100.0f) / 100.0f, ch = h * std::clamp(height_percent, 1.0f, 100.0f) / 100.0f;
    const float r = std::max(2.0f, static_cast<float>(radius));
    const Image src = img;
    uint8_t* d = img.data();
    const bool right = corner == 1 || corner == 3, bottom = corner >= 2;
    // In corner coordinates (distance from the two edges that meet at the
    // curled corner) the fold line is fx/cw + fy/ch = 1; the triangle inside
    // it is curled away.
    const float nx = 1.0f / cw, ny = 1.0f / ch, nlen = std::hypot(nx, ny);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float fx = static_cast<float>(right ? (w - 1 - x) : x), fy = static_cast<float>(bottom ? (h - 1 - y) : y);
            const float g = fx * nx + fy * ny;
            if (g >= 1.0f) continue;
            const float dist = (1.0f - g) / nlen;   // distance inside the fold
            uint8_t* p = d + (static_cast<size_t>(y) * w + x) * 4;
            if (dist > r * kPi) {
                if (transparent_fill) { p[0] = p[1] = p[2] = 0; p[3] = 0; }
                else { p[0] = fill.r; p[1] = fill.g; p[2] = fill.b; p[3] = 255; }
                continue;
            }
            // The curled band shows the back of the page: mirror the source
            // across the fold and shade it like a cylinder.
            const float k = 2.0f * (1.0f - g) / (nlen * nlen);
            const float mx = fx + nx * k, my = fy + ny * k;
            const int sxi = std::clamp(static_cast<int>(right ? (w - 1 - mx) : mx), 0, w - 1), syi = std::clamp(static_cast<int>(bottom ? (h - 1 - my) : my), 0, h - 1);
            const uint8_t* s = src.data() + (static_cast<size_t>(syi) * w + sxi) * 4;
            const float shade = 0.6f + 0.4f * std::sin(dist / r);
            for (int c = 0; c < 3; ++c) {
                const int b = c == 0 ? back.r : c == 1 ? back.g : back.b;
                p[c] = clamp8((b * 0.8f + s[c] * 0.2f) * shade);
            }
            p[3] = 255;
        }
}

}  // namespace firn::effects
