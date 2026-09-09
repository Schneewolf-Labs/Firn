#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

#include "effects_util.h"
#include "firn/adjust.h"
#include "firn/effects.h"
#include "firn/raster.h"

namespace firn::effects {
using detail::clamp8;
using detail::remap;
using detail::rgb_pass;

namespace {
constexpr float kPi = 3.14159265358979f;

std::vector<float> luma_map(const Image& img) {
    std::vector<float> l(static_cast<size_t>(img.width()) * img.height());
    for (size_t i = 0; i < l.size(); ++i) l[i] = detail::luma_of(img.data() + i * 4);
    return l;
}

void blur_map(std::vector<float>& m, int w, int h, float radius) {
    if (radius <= 0.0f) return;
    Image tmp(w, h);
    for (size_t i = 0; i < m.size(); ++i) { uint8_t* p = tmp.data() + i * 4; p[0] = p[1] = p[2] = clamp8(m[i]); p[3] = 255; }
    raster::gaussian_blur(tmp, radius);
    for (size_t i = 0; i < m.size(); ++i) m[i] = tmp.data()[i * 4];
}

// Value noise in 0..1 with a few octaves.
struct Noise {
    std::vector<float> grid;
    int gw, gh;
    Noise(int w, int h, uint32_t seed) : gw(std::max(2, w)), gh(std::max(2, h)) {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        grid.resize(static_cast<size_t>(gw) * gh);
        for (float& v : grid) v = u(rng);
    }
    float at(float x, float y) const {   // x, y in grid units
        const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        const float fx = x - x0, fy = y - y0;
        auto g = [&](int i, int j) { return grid[static_cast<size_t>(((j % gh) + gh) % gh) * gw + ((i % gw) + gw) % gw]; };
        const float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
        return (g(x0, y0) * (1 - sx) + g(x0 + 1, y0) * sx) * (1 - sy) + (g(x0, y0 + 1) * (1 - sx) + g(x0 + 1, y0 + 1) * sx) * sy;
    }
    float fbm(float x, float y, int octaves) const {
        float v = 0, amp = 0.5f, sum = 0;
        for (int o = 0; o < octaves; ++o) { v += at(x, y) * amp; sum += amp; x *= 2; y *= 2; amp *= 0.5f; }
        return v / sum;
    }
};

// Shades img by a height map lit from `angle` (degrees, 0 = from the right,
// counter-clockwise) with `depth` scaling the slope; `color_amount` mixes
// the light color into the result.
void bump_shade(Image& img, const std::vector<float>& height, float depth, float angle, Color color, float color_amount, float ambient = 0.6f) {
    const int w = img.width(), h = img.height();
    const float rad = angle * kPi / 180.0f;
    const float lx = std::cos(rad), ly = -std::sin(rad), lz = 0.7f;
    const float ln = std::hypot(std::hypot(lx, ly), lz);
    uint8_t* d = img.data();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            const float hl = height[static_cast<size_t>(y) * w + std::max(0, x - 1)], hr = height[static_cast<size_t>(y) * w + std::min(w - 1, x + 1)];
            const float hu = height[static_cast<size_t>(std::max(0, y - 1)) * w + x], hd = height[static_cast<size_t>(std::min(h - 1, y + 1)) * w + x];
            const float nx = -(hr - hl) * depth, ny = -(hd - hu) * depth, nz = 1.0f;
            const float nn = std::hypot(std::hypot(nx, ny), nz);
            const float diffuse = std::max(0.0f, (nx * lx + ny * ly + nz * lz) / (nn * ln));
            const float shade = ambient + (1.0f - ambient) * diffuse * 1.4f;
            uint8_t* p = d + i * 4;
            if (!p[3]) continue;
            const int col[3] = {color.r, color.g, color.b};
            for (int c = 0; c < 3; ++c) p[c] = clamp8((p[c] + (col[c] - p[c]) * color_amount) * shade);
        }
}

std::vector<float> edge_map(const Image& img) {
    const int w = img.width(), h = img.height();
    const std::vector<float> l = luma_map(img);
    std::vector<float> e(l.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            auto at = [&](int i, int j) { return l[static_cast<size_t>(std::clamp(j, 0, h - 1)) * w + std::clamp(i, 0, w - 1)]; };
            const float gx = at(x + 1, y - 1) + 2 * at(x + 1, y) + at(x + 1, y + 1) - at(x - 1, y - 1) - 2 * at(x - 1, y) - at(x - 1, y + 1);
            const float gy = at(x - 1, y + 1) + 2 * at(x, y + 1) + at(x + 1, y + 1) - at(x - 1, y - 1) - 2 * at(x, y - 1) - at(x + 1, y - 1);
            e[static_cast<size_t>(y) * w + x] = std::hypot(gx, gy) / 4.0f;
        }
    return e;
}

void set_rgb(uint8_t* p, float r, float g, float b) { p[0] = clamp8(r); p[1] = clamp8(g); p[2] = clamp8(b); }
}  // namespace

void aged_newspaper(Image& img, int amount) {
    const float a = std::clamp(amount, 1, 100) / 100.0f;
    adjust::sepia(img, static_cast<int>(60 * a));
    raster::gaussian_blur(img, 0.5f + 1.5f * a);
    add_noise(img, static_cast<int>(25 * a), true, true, 3);
    adjust::apply_lut(img, adjust::brightness_contrast_lut(static_cast<int>(20 * a), static_cast<int>(-15 * a)));
}

void balls_and_bubbles(Image& img, int count, int min_size, int max_size, int opacity, bool bubbles, Color color, uint32_t seed) {
    const int w = img.width(), h = img.height();
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> ux(0, static_cast<float>(w)), uy(0, static_cast<float>(h)), us(static_cast<float>(std::min(min_size, max_size)), static_cast<float>(std::max(min_size, max_size)));
    const float op = std::clamp(opacity, 0, 100) / 100.0f;
    for (int n = 0; n < std::clamp(count, 1, 500); ++n) {
        const float cx = ux(rng), cy = uy(rng), r = std::max(2.0f, us(rng));
        const int x0 = std::max(0, static_cast<int>(cx - r)), x1 = std::min(w - 1, static_cast<int>(cx + r));
        const int y0 = std::max(0, static_cast<int>(cy - r)), y1 = std::min(h - 1, static_cast<int>(cy + r));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const float dx = (x - cx) / r, dy = (y - cy) / r;
                const float d2 = dx * dx + dy * dy;
                if (d2 > 1.0f) continue;
                const float nz = std::sqrt(1.0f - d2);
                const float light = std::max(0.0f, (-dx * 0.5f - dy * 0.5f + nz * 0.7f) / 1.0f);
                const float spec = std::pow(std::max(0.0f, light), 24.0f);
                uint8_t* p = img.data() + (static_cast<size_t>(y) * w + x) * 4;
                float t = op;
                Color c = color;
                if (bubbles) {
                    // Bubbles: refract the background a little and shade the rim.
                    const float rim = std::pow(d2, 3.0f);
                    const int sx = std::clamp(static_cast<int>(cx + dx * r * 0.85f), 0, w - 1), sy = std::clamp(static_cast<int>(cy + dy * r * 0.85f), 0, h - 1);
                    const uint8_t* s = img.data() + (static_cast<size_t>(sy) * w + sx) * 4;
                    c = {s[0], s[1], s[2], 255};
                    t = op * (0.5f + 0.5f * rim);
                    set_rgb(p, p[0] + (c.r * (0.7f + 0.3f * light) + 255 * spec - p[0]) * t, p[1] + (c.g * (0.7f + 0.3f * light) + 255 * spec - p[1]) * t, p[2] + (c.b * (0.7f + 0.3f * light) + 255 * spec - p[2]) * t);
                } else {
                    set_rgb(p, p[0] + (c.r * (0.3f + 0.7f * light) + 255 * spec - p[0]) * t, p[1] + (c.g * (0.3f + 0.7f * light) + 255 * spec - p[1]) * t, p[2] + (c.b * (0.3f + 0.7f * light) + 255 * spec - p[2]) * t);
                }
            }
    }
}

void colored_edges(Image& img, int luminance, int blur, Color color) {
    if (blur > 0) raster::gaussian_blur(img, static_cast<float>(blur));
    const std::vector<float> e = edge_map(img);
    const float lum = std::clamp(luminance, 0, 100) / 100.0f;
    uint8_t* d = img.data();
    for (size_t i = 0; i < e.size(); ++i) {
        const float t = std::clamp(e[i] / 64.0f, 0.0f, 1.0f);
        uint8_t* p = d + i * 4;
        set_rgb(p, p[0] * lum * (1 - t) + color.r * t, p[1] * lum * (1 - t) + color.g * t, p[2] * lum * (1 - t) + color.b * t);
    }
}

void colored_foil(Image& img, int blur, int detail, Color color, float angle) {
    std::vector<float> hmap = luma_map(img);
    blur_map(hmap, img.width(), img.height(), static_cast<float>(blur));
    // Foil: a rainbow of hues by slope, lit strongly.
    bump_shade(img, hmap, 0.02f * std::clamp(detail, 1, 100), angle, color, 0.6f, 0.3f);
    adjust::hsl_adjust(img, 0, 40, 0);
}

void contours(Image& img, int luminance, int blur, int detail, Color color) {
    if (blur > 0) raster::gaussian_blur(img, static_cast<float>(blur));
    const int w = img.width(), h = img.height();
    const std::vector<float> l = luma_map(img);
    const int levels = std::clamp(detail, 2, 20);
    const float lum = std::clamp(luminance, 0, 100) / 100.0f;
    uint8_t* d = img.data();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            auto band = [&](int i, int j) { return static_cast<int>(l[static_cast<size_t>(std::clamp(j, 0, h - 1)) * w + std::clamp(i, 0, w - 1)] / 256.0f * levels); };
            const int b = band(x, y);
            const bool edge = band(x + 1, y) != b || band(x, y + 1) != b;
            uint8_t* p = d + (static_cast<size_t>(y) * w + x) * 4;
            if (edge) set_rgb(p, color.r, color.g, color.b);
            else set_rgb(p, p[0] * lum, p[1] * lum, p[2] * lum);
        }
}

void enamel(Image& img, int blur, int detail, int density, float angle, Color color) {
    std::vector<float> hmap = luma_map(img);
    blur_map(hmap, img.width(), img.height(), static_cast<float>(std::max(1, blur)));
    raster::gaussian_blur(img, std::max(1.0f, blur * 0.5f));
    bump_shade(img, hmap, 0.03f * std::clamp(detail, 1, 100), angle, color, std::clamp(density, 0, 100) / 400.0f, 0.55f);
}

void glowing_edges(Image& img, int intensity, int sharpness) {
    const std::vector<float> e = edge_map(img);
    const float k = std::clamp(intensity, 1, 100) / 25.0f;
    const float power = 1.0f + std::clamp(sharpness, 1, 100) / 50.0f;
    uint8_t* d = img.data();
    for (size_t i = 0; i < e.size(); ++i) {
        const float t = std::pow(std::clamp(e[i] / 64.0f, 0.0f, 1.0f), 1.0f / power) * k;
        uint8_t* p = d + i * 4;
        set_rgb(p, p[0] * t, p[1] * t, p[2] * t);
    }
}

void hot_wax(Image& img, Color wax) {
    std::vector<float> hmap = luma_map(img);
    blur_map(hmap, img.width(), img.height(), 2.0f);
    bump_shade(img, hmap, 0.6f, 315.0f, wax, 0.7f, 0.5f);
}

void magnifying_lens(Image& img, float cx_percent, float cy_percent, float size_percent, int refraction, int shading) {
    const int w = img.width(), h = img.height();
    const float cx = w * cx_percent / 100.0f, cy = h * cy_percent / 100.0f;
    const float radius = std::max(2.0f, std::min(w, h) * size_percent / 100.0f);
    const float k = std::clamp(refraction, 0, 100) / 100.0f;
    remap(img, [&](float x, float y, float& sx, float& sy) {
        const float dx = x - cx, dy = y - cy;
        const float d = std::hypot(dx, dy);
        if (d >= radius) { sx = x; sy = y; return; }
        const float t = d / radius;
        const float f = 1.0f - k * (1.0f - t * t) * 0.7f;   // stronger magnification at the center
        sx = cx + dx * f; sy = cy + dy * f;
    });
    // Rim shading and a highlight.
    const float sh = std::clamp(shading, 0, 100) / 100.0f;
    uint8_t* d = img.data();
    for (int y = std::max(0, static_cast<int>(cy - radius)); y <= std::min(h - 1, static_cast<int>(cy + radius)); ++y)
        for (int x = std::max(0, static_cast<int>(cx - radius)); x <= std::min(w - 1, static_cast<int>(cx + radius)); ++x) {
            const float dx = (x - cx) / radius, dy = (y - cy) / radius;
            const float d2 = dx * dx + dy * dy;
            if (d2 > 1.0f) continue;
            const float rim = std::pow(d2, 4.0f) * sh;
            const float spec = std::pow(std::max(0.0f, -dx * 0.6f - dy * 0.6f + std::sqrt(1 - d2) * 0.5f), 30.0f) * sh;
            uint8_t* p = d + (static_cast<size_t>(y) * w + x) * 4;
            set_rgb(p, p[0] * (1 - rim) + 255 * spec, p[1] * (1 - rim) + 255 * spec, p[2] * (1 - rim) + 255 * spec);
        }
}

void neon_glow(Image& img, int detail, int opacity) {
    const std::vector<float> e = edge_map(img);
    const Image src = img;
    const float k = std::clamp(detail, 1, 100) / 30.0f;
    const float op = std::clamp(opacity, 0, 100) / 100.0f;
    uint8_t* d = img.data();
    for (size_t i = 0; i < e.size(); ++i) {
        const float t = std::clamp(e[i] / 48.0f * k, 0.0f, 1.0f);
        const uint8_t* s = src.data() + i * 4;
        uint8_t* p = d + i * 4;
        // Saturated source hue at edge strength over black.
        const adjust::HSL hsl = adjust::rgb_to_hsl(s[0], s[1], s[2]);
        uint8_t r, g, b;
        adjust::hsl_to_rgb({hsl.h, std::max(hsl.s, 0.8f), 0.5f * t}, &r, &g, &b);
        set_rgb(p, p[0] * (1 - op) + r * op, p[1] * (1 - op) + g * op, p[2] * (1 - op) + b * op);
    }
    raster::gaussian_blur(img, 1.0f);
}

void topography(Image& img, int width, int density, float angle, Color color) {
    const int w = img.width(), h = img.height();
    std::vector<float> l = luma_map(img);
    blur_map(l, w, h, 1.5f);
    const int levels = std::clamp(density, 2, 32);
    std::vector<float> stepped(l.size());
    for (size_t i = 0; i < l.size(); ++i) stepped[i] = std::floor(l[i] / 256.0f * levels) * (256.0f / levels);
    // Each band is a terrace; shade by the terrace edges.
    bump_shade(img, stepped, 0.15f * std::clamp(width, 1, 100) / 10.0f, angle, color, 0.25f, 0.6f);
}

void lights(Image& img, const Light* ls, int count, int darkness) {
    const int w = img.width(), h = img.height();
    const float dark = 1.0f - std::clamp(darkness, 0, 100) / 100.0f;
    uint8_t* d = img.data();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float acc[3] = {dark, dark, dark};
            for (int i = 0; i < count; ++i) {
                const Light& L = ls[i];
                if (!L.on) continue;
                const float lx = w * L.x / 100.0f, ly = h * L.y / 100.0f;
                const float dx = x - lx, dy = y - ly;
                const float dist = std::hypot(dx, dy) / std::max(w, h);
                const float ang = std::atan2(dy, dx) * 180.0f / kPi;
                float da = std::fmod(std::abs(ang - L.direction) + 360.0f, 360.0f);
                if (da > 180.0f) da = 360.0f - da;
                const float cone = std::clamp(1.0f - da / std::max(1.0f, L.cone * 0.5f), 0.0f, 1.0f);
                const float falloff = std::exp(-dist * 3.0f);
                const float in = L.intensity / 100.0f * 1.5f * cone * falloff;
                acc[0] += in * L.color.r / 255.0f; acc[1] += in * L.color.g / 255.0f; acc[2] += in * L.color.b / 255.0f;
            }
            uint8_t* p = d + (static_cast<size_t>(y) * w + x) * 4;
            set_rgb(p, p[0] * acc[0], p[1] * acc[1], p[2] * acc[2]);
        }
}

void blinds(Image& img, int width, int opacity, bool horizontal, bool light_from_left, Color color) {
    const int w = img.width(), h = img.height();
    const int period = std::max(2, width);
    const float op = std::clamp(opacity, 0, 100) / 100.0f;
    uint8_t* d = img.data();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int v = horizontal ? y : x;
            float t = static_cast<float>(v % period) / period;
            if (!light_from_left) t = 1.0f - t;
            const float shade = op * (1.0f - t);   // dark at the slat's shadowed edge
            uint8_t* p = d + (static_cast<size_t>(y) * w + x) * 4;
            set_rgb(p, p[0] + (color.r - p[0]) * shade, p[1] + (color.g - p[1]) * shade, p[2] + (color.b - p[2]) * shade);
        }
}

void leather(Image& img, bool rough, int color_amount, float angle, int blur, int transparency, Color color, uint32_t seed) {
    const int w = img.width(), h = img.height();
    const Noise n(rough ? 48 : 24, rough ? 48 : 24, seed);
    std::vector<float> hmap(static_cast<size_t>(w) * h);
    const float cell = rough ? 6.0f : 14.0f;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) hmap[static_cast<size_t>(y) * w + x] = n.fbm(x / cell, y / cell, rough ? 4 : 3) * 255.0f;
    blur_map(hmap, w, h, static_cast<float>(blur));
    const Image before = img;
    bump_shade(img, hmap, rough ? 1.2f : 0.6f, angle, color, std::clamp(color_amount, 0, 100) / 100.0f, 0.5f);
    const float keep = std::clamp(transparency, 0, 100) / 100.0f;
    if (keep > 0) { uint8_t* d = img.data(); const uint8_t* s = before.data(); for (size_t i = 0; i < img.size_bytes(); i += 4) for (int c = 0; c < 3; ++c) d[i + c] = clamp8(d[i + c] + (s[i + c] - d[i + c]) * keep); }
}

void fur(Image& img, int blur, int density, int length, int transparency, uint32_t seed) {
    const int w = img.width(), h = img.height();
    if (blur > 0) raster::gaussian_blur(img, static_cast<float>(blur));
    const Image src = img;
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> ux(0, w - 1), uy(0, h - 1);
    std::uniform_real_distribution<float> ua(-0.6f, 0.6f), ul(0.5f, 1.0f);
    const int strokes = static_cast<int>(static_cast<long>(w) * h * std::clamp(density, 1, 100) / 400);
    const float op = 1.0f - std::clamp(transparency, 0, 100) / 100.0f;
    const int len = std::max(2, length);
    for (int i = 0; i < strokes; ++i) {
        const int x0 = ux(rng), y0 = uy(rng);
        const Color c = src.get(x0, y0);
        const float ang = -kPi / 2 + ua(rng);
        const int l = static_cast<int>(len * ul(rng));
        for (int k = 0; k < l; ++k) {
            const int x = x0 + static_cast<int>(std::cos(ang) * k), y = y0 + static_cast<int>(std::sin(ang) * k);
            if (x < 0 || y < 0 || x >= w || y >= h) break;
            const float fade = 1.0f - static_cast<float>(k) / l;
            raster::blend_over(img, x, y, {clamp8(c.r * (1.0f + 0.2f * fade)), clamp8(c.g * (1.0f + 0.2f * fade)), clamp8(c.b * (1.0f + 0.2f * fade)), 255}, op * fade);
        }
    }
}

void mosaic_antique(Image& img, int columns, int rows, int symmetric, int diffusion, int grout_width, int grout_transparency) {
    (void)symmetric;
    const int w = img.width(), h = img.height();
    const float cw = static_cast<float>(w) / std::max(1, columns), ch = static_cast<float>(h) / std::max(1, rows);
    const Image src = img;
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    const float diff = std::clamp(diffusion, 0, 100) / 100.0f * 40.0f;
    const float g = std::clamp(grout_width, 0, 100) / 100.0f * std::min(cw, ch) * 0.5f;
    const float gop = 1.0f - std::clamp(grout_transparency, 0, 100) / 100.0f;
    uint8_t* d = img.data();
    for (int cy = 0; cy < std::max(1, rows); ++cy)
        for (int cx = 0; cx < std::max(1, columns); ++cx) {
            // Average tile color with an antique tint shift.
            float acc[3] = {0, 0, 0}; int n = 0;
            const int x0 = static_cast<int>(cx * cw), x1 = std::min(w, static_cast<int>((cx + 1) * cw)), y0 = static_cast<int>(cy * ch), y1 = std::min(h, static_cast<int>((cy + 1) * ch));
            for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) { const uint8_t* s = src.data() + (static_cast<size_t>(y) * w + x) * 4; for (int c = 0; c < 3; ++c) acc[c] += s[c]; ++n; }
            if (!n) continue;
            const float shift = u(rng) * diff;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    const float ex = std::min(x - x0, x1 - 1 - x), ey = std::min(y - y0, y1 - 1 - y);
                    const bool grout = ex < g || ey < g;
                    uint8_t* p = d + (static_cast<size_t>(y) * w + x) * 4;
                    if (grout) set_rgb(p, p[0] * (1 - gop) + 60 * gop, p[1] * (1 - gop) + 55 * gop, p[2] * (1 - gop) + 50 * gop);
                    else set_rgb(p, acc[0] / n + shift, acc[1] / n + shift * 0.8f, acc[2] / n + shift * 0.6f);
                }
        }
}

void mosaic_glass(Image& img, int columns, int rows, int curvature, int edge_width, int grout_transparency) {
    const int w = img.width(), h = img.height();
    const float cw = static_cast<float>(w) / std::max(1, columns), ch = static_cast<float>(h) / std::max(1, rows);
    const float k = std::clamp(curvature, 0, 100) / 100.0f;
    remap(img, [&](float x, float y, float& sx, float& sy) {
        const float cx = (std::floor(x / cw) + 0.5f) * cw, cy = (std::floor(y / ch) + 0.5f) * ch;
        const float dx = (x - cx) / (cw * 0.5f), dy = (y - cy) / (ch * 0.5f);
        sx = cx + dx * cw * 0.5f * (1.0f - k * 0.5f * (1.0f - dx * dx));
        sy = cy + dy * ch * 0.5f * (1.0f - k * 0.5f * (1.0f - dy * dy));
    });
    const float g = std::clamp(edge_width, 0, 100) / 100.0f * std::min(cw, ch) * 0.25f;
    const float gop = 1.0f - std::clamp(grout_transparency, 0, 100) / 100.0f;
    uint8_t* d = img.data();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float ex = std::fmod(static_cast<float>(x), cw), ey = std::fmod(static_cast<float>(y), ch);
            const float e = std::min({ex, cw - ex, ey, ch - ey});
            if (e >= g) continue;
            const float t = (1.0f - e / std::max(g, 1.0f)) * gop;
            uint8_t* p = d + (static_cast<size_t>(y) * w + x) * 4;
            set_rgb(p, p[0] * (1 - t) + 230 * t, p[1] * (1 - t) + 230 * t, p[2] * (1 - t) + 235 * t);
        }
}

void polished_stone(Image& img, int blur, int detail, float angle, int color_amount, Color color) {
    std::vector<float> hmap = luma_map(img);
    blur_map(hmap, img.width(), img.height(), static_cast<float>(std::max(1, blur)));
    raster::gaussian_blur(img, 1.0f);
    bump_shade(img, hmap, 0.04f * std::clamp(detail, 1, 100), angle, color, std::clamp(color_amount, 0, 100) / 100.0f, 0.45f);
    effects::sharpen(img);
}

void sandstone(Image& img, int blur, int detail, float angle, Color color, uint32_t seed) {
    const int w = img.width(), h = img.height();
    std::vector<float> hmap = luma_map(img);
    blur_map(hmap, w, h, static_cast<float>(std::max(1, blur)));
    const Noise n(64, 64, seed);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) hmap[static_cast<size_t>(y) * w + x] += (n.at(x * 0.7f, y * 0.7f) - 0.5f) * std::clamp(detail, 1, 100) * 1.5f;
    bump_shade(img, hmap, 0.35f, angle, color, 0.5f, 0.5f);
}

void sculpture(Image& img, int smoothness, int depth, float angle, Color color) {
    std::vector<float> hmap = luma_map(img);
    blur_map(hmap, img.width(), img.height(), std::clamp(smoothness, 0, 100) / 10.0f);
    bump_shade(img, hmap, 0.02f * std::clamp(depth, 1, 100), angle, color, 0.7f, 0.35f);
}

void soft_plastic(Image& img, int blur, int detail, int density, float angle, Color color) {
    std::vector<float> hmap = luma_map(img);
    blur_map(hmap, img.width(), img.height(), static_cast<float>(std::max(2, blur)));
    raster::gaussian_blur(img, 2.0f);
    bump_shade(img, hmap, 0.015f * std::clamp(detail, 1, 100), angle, color, std::clamp(density, 0, 100) / 200.0f, 0.65f);
}

void straw_wall(Image& img, int blur, int detail, int density, float angle, Color color, uint32_t seed) {
    const int w = img.width(), h = img.height();
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> ux(0, w - 1), uy(0, h - 1);
    std::uniform_real_distribution<float> ua(-0.3f, 0.3f);
    std::vector<float> hmap(static_cast<size_t>(w) * h, 0.0f);
    const int straws = static_cast<int>(static_cast<long>(w) * h * std::clamp(density, 1, 100) / 2000);
    const int len = std::max(4, std::clamp(detail, 1, 100) / 2);
    for (int i = 0; i < straws; ++i) {
        const int x0 = ux(rng), y0 = uy(rng);
        const float ang = (i % 2 ? 0.0f : kPi / 2) + ua(rng);
        for (int k = 0; k < len; ++k) {
            const int x = x0 + static_cast<int>(std::cos(ang) * k), y = y0 + static_cast<int>(std::sin(ang) * k);
            if (x < 0 || y < 0 || x >= w || y >= h) break;
            hmap[static_cast<size_t>(y) * w + x] += 40.0f;
        }
    }
    blur_map(hmap, w, h, static_cast<float>(std::max(0, blur)) * 0.5f);
    bump_shade(img, hmap, 0.5f, angle, color, 0.35f, 0.5f);
}

void texture(Image& img, const Image& bump, int size_percent, int smoothness, int depth, float angle, Color color) {
    const int w = img.width(), h = img.height();
    std::vector<float> hmap(static_cast<size_t>(w) * h);
    if (bump.empty()) {
        const Noise n(32, 32, 5);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) hmap[static_cast<size_t>(y) * w + x] = n.fbm(x / 12.0f, y / 12.0f, 3) * 255.0f;
    } else {
        const float scale = std::clamp(size_percent, 10, 400) / 100.0f;
        const int bw = std::max(1, static_cast<int>(bump.width() * scale)), bh = std::max(1, static_cast<int>(bump.height() * scale));
        const Image tile = raster::resample(bump, bw, bh, raster::Filter::Bilinear);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) hmap[static_cast<size_t>(y) * w + x] = detail::luma_of(tile.data() + (static_cast<size_t>(y % bh) * bw + x % bw) * 4);
    }
    blur_map(hmap, w, h, std::clamp(smoothness, 0, 100) / 20.0f);
    bump_shade(img, hmap, 0.02f * std::clamp(depth, 1, 100), angle, color, 0.3f, 0.55f);
}

void tiles(Image& img, int shape, int size, int border, int smoothness, int depth, float angle, Color color) {
    const int w = img.width(), h = img.height();
    const float cell = static_cast<float>(std::max(4, size));
    const float bw = std::clamp(border, 0, 100) / 100.0f * cell * 0.5f;
    std::vector<float> hmap(static_cast<size_t>(w) * h, 255.0f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float e;
            if (shape == 1) {   // hexagons (approximated by offset rows)
                const float rowh = cell * 0.866f;
                const int row = static_cast<int>(y / rowh);
                const float ox = (row % 2) ? cell * 0.5f : 0.0f;
                const float fx = std::fmod(x + ox, cell), fy = std::fmod(static_cast<float>(y), rowh);
                e = std::min({fx, cell - fx, fy, rowh - fy});
            } else if (shape == 2) {   // triangles: squares split by a diagonal
                const float fx = std::fmod(static_cast<float>(x), cell), fy = std::fmod(static_cast<float>(y), cell);
                e = std::min({fx, cell - fx, fy, cell - fy, std::abs(fx - fy) * 0.7f});
            } else {
                const float fx = std::fmod(static_cast<float>(x), cell), fy = std::fmod(static_cast<float>(y), cell);
                e = std::min({fx, cell - fx, fy, cell - fy});
            }
            hmap[static_cast<size_t>(y) * w + x] = std::clamp(e / std::max(bw, 1.0f), 0.0f, 1.0f) * 255.0f;
        }
    blur_map(hmap, w, h, std::clamp(smoothness, 0, 100) / 25.0f);
    bump_shade(img, hmap, 0.02f * std::clamp(depth, 1, 100), angle, color, 0.2f, 0.6f);
}

void weave(Image& img, int gap, int width, int opacity, Color gap_color, Color weave_color, bool fill_gaps) {
    const int w = img.width(), h = img.height();
    const int period = std::max(2, width + gap);
    const float op = std::clamp(opacity, 0, 100) / 100.0f;
    uint8_t* d = img.data();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int fx = x % period, fy = y % period;
            const bool in_h = fy < width, in_v = fx < width;
            const int cx = x / period, cy = y / period;
            uint8_t* p = d + (static_cast<size_t>(y) * w + x) * 4;
            if (!in_h && !in_v) {
                if (fill_gaps) set_rgb(p, p[0] + (gap_color.r - p[0]) * op, p[1] + (gap_color.g - p[1]) * op, p[2] + (gap_color.b - p[2]) * op);
                continue;
            }
            // Over/under: alternate which strip is on top per cell; shade the one underneath.
            const bool h_on_top = ((cx + cy) % 2) == 0;
            float shade;
            if (in_h && in_v) shade = 1.0f;
            else if (in_h) shade = h_on_top ? 1.0f : 0.65f;
            else shade = h_on_top ? 0.65f : 1.0f;
            const float edge = in_h ? std::min(fy, width - 1 - fy) : std::min(fx, width - 1 - fx);
            shade *= 0.8f + 0.2f * std::min(1.0f, edge / 2.0f);
            set_rgb(p, (p[0] + (weave_color.r - p[0]) * op) * shade, (p[1] + (weave_color.g - p[1]) * op) * shade, (p[2] + (weave_color.b - p[2]) * op) * shade);
        }
}

// --- Art media -------------------------------------------------------------------

namespace {
// Pencil-style strokes: edges darken along a hatch direction; base is
// lightened source or white.
void hatch_edges(Image& img, int detail, int opacity, bool keep_color, Color ink) {
    const std::vector<float> e = edge_map(img);
    const int w = img.width(), h = img.height();
    const float k = std::clamp(detail, 1, 100) / 40.0f;
    const float op = std::clamp(opacity, 0, 100) / 100.0f;
    const Image src = img;
    uint8_t* d = img.data();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            const float l = detail::luma_of(src.data() + i * 4);
            const float hatch = 0.85f + 0.15f * std::sin((x + y) * 0.9f);   // diagonal grain
            const float dark = std::clamp(e[i] / 48.0f * k + (1.0f - l / 255.0f) * 0.5f, 0.0f, 1.0f) * hatch;
            uint8_t* p = d + i * 4;
            const int base[3] = {keep_color ? src.data()[i * 4] : 255, keep_color ? src.data()[i * 4 + 1] : 255, keep_color ? src.data()[i * 4 + 2] : 255};
            const int inkc[3] = {ink.r, ink.g, ink.b};
            for (int c = 0; c < 3; ++c) {
                const float v = base[c] + (inkc[c] - base[c]) * dark;
                p[c] = clamp8(p[c] + (v - p[c]) * op);
            }
        }
}
}  // namespace

void black_pencil(Image& img, int detail, int opacity) { hatch_edges(img, detail, opacity, false, {0, 0, 0, 255}); }

void brush_strokes(Image& img, int length, int density, int width, int opacity, uint32_t seed) {
    const int w = img.width(), h = img.height();
    const Image src = img;
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> ux(0, w - 1), uy(0, h - 1);
    std::uniform_real_distribution<float> ua(0.0f, kPi);
    const int strokes = static_cast<int>(static_cast<long>(w) * h * std::clamp(density, 1, 100) / 3000);
    const float op = std::clamp(opacity, 0, 100) / 100.0f;
    const int len = std::max(2, length), half = std::max(1, width / 2);
    for (int i = 0; i < strokes; ++i) {
        const int x0 = ux(rng), y0 = uy(rng);
        const Color c = src.get(x0, y0);
        const float ang = ua(rng);
        const float dx = std::cos(ang), dy = std::sin(ang);
        for (int k = -len / 2; k <= len / 2; ++k)
            for (int t = -half; t <= half; ++t) {
                const int x = x0 + static_cast<int>(dx * k - dy * t), y = y0 + static_cast<int>(dy * k + dx * t);
                if (x < 0 || y < 0 || x >= w || y >= h) continue;
                raster::blend_over(img, x, y, c, op * (1.0f - std::abs(t) / (half + 1.0f)));
            }
    }
}

void charcoal(Image& img, int detail, int opacity) {
    hatch_edges(img, detail, opacity, false, {20, 20, 20, 255});
    raster::gaussian_blur(img, 0.8f);
}

void colored_chalk(Image& img, int detail, int opacity) {
    hatch_edges(img, detail, opacity, true, {40, 40, 40, 255});
    add_noise(img, 12, true, true, 9);
    adjust::hsl_adjust(img, 0, 20, 10);
}

void colored_pencil(Image& img, int detail, int opacity) {
    hatch_edges(img, detail, opacity, true, {30, 30, 30, 255});
    adjust::hsl_adjust(img, 0, 15, 20);
}

void pencil(Image& img, int luminance, int blur, Color color) {
    if (blur > 0) raster::gaussian_blur(img, static_cast<float>(blur));
    const std::vector<float> e = edge_map(img);
    const float lum = std::clamp(luminance, 0, 100) / 100.0f;
    uint8_t* d = img.data();
    for (size_t i = 0; i < e.size(); ++i) {
        const float t = std::clamp(e[i] / 40.0f, 0.0f, 1.0f);
        uint8_t* p = d + i * 4;
        const float base = 255.0f * lum;
        set_rgb(p, base + (color.r - base) * t, base + (color.g - base) * t, base + (color.b - base) * t);
    }
}

void user_defined_filter(Image& img, const float kernel[25], float divisor, float bias) {
    if (divisor == 0.0f) divisor = 1.0f;
    rgb_pass(img, [&](int x, int y, auto at, float* out) {
        for (int c = 0; c < 3; ++c) out[c] = 0.0f;
        for (int j = -2; j <= 2; ++j)
            for (int i = -2; i <= 2; ++i) {
                const float k = kernel[(j + 2) * 5 + (i + 2)];
                if (k == 0.0f) continue;
                const uint8_t* s = at(x + i, y + j);
                for (int c = 0; c < 3; ++c) out[c] += s[c] * k;
            }
        for (int c = 0; c < 3; ++c) out[c] = out[c] / divisor + bias;
    });
}

}  // namespace firn::effects
