#include "firn/effects.h"
#include "effects_util.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "firn/raster.h"

namespace firn::effects {
using detail::clamp8;
using detail::rgb_pass;
using detail::remap;


void convolve3(Image& img, const float kernel[9], float divisor, float bias) {
    if (divisor == 0.0f) divisor = 1.0f;
    rgb_pass(img, [&](int x, int y, auto at, float* out) {
        for (int c = 0; c < 3; ++c) {
            float acc = 0.0f;
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i) acc += at(x + i, y + j)[c] * kernel[(j + 1) * 3 + (i + 1)];
            out[c] = acc / divisor + bias;
        }
    });
}

void sharpen(Image& img) {
    static const float k[9] = {0, -1, 0, -1, 5, -1, 0, -1, 0};
    convolve3(img, k);
}

void sharpen_more(Image& img) {
    static const float k[9] = {-1, -1, -1, -1, 9, -1, -1, -1, -1};
    convolve3(img, k);
}

void blur_more(Image& img) {
    static const float k[9] = {1, 1, 1, 1, 1, 1, 1, 1, 1};
    convolve3(img, k, 9.0f);
}

void soften(Image& img) {
    static const float k[9] = {1, 1, 1, 1, 4, 1, 1, 1, 1};
    convolve3(img, k, 12.0f);
}

void soften_more(Image& img) {
    static const float k[9] = {1, 2, 1, 2, 4, 2, 1, 2, 1};
    convolve3(img, k, 16.0f);
}

void unsharp_mask(Image& img, float radius, int strength, int clipping) {
    Image blurred = img;
    raster::gaussian_blur(blurred, radius);
    const float amount = strength / 100.0f;
    uint8_t* p = img.data();
    const uint8_t* b = blurred.data();
    for (size_t i = 0; i < img.size_bytes(); i += 4)
        for (int c = 0; c < 3; ++c) {
            const int diff = p[i + c] - b[i + c];
            if (std::abs(diff) < clipping) continue;
            p[i + c] = clamp8(p[i + c] + diff * amount);
        }
}

void median(Image& img, int radius) {
    radius = std::max(1, radius);
    std::vector<uint8_t> win;
    rgb_pass(img, [&](int x, int y, auto at, float* out) {
        for (int c = 0; c < 3; ++c) {
            win.clear();
            for (int j = -radius; j <= radius; ++j)
                for (int i = -radius; i <= radius; ++i) win.push_back(at(x + i, y + j)[c]);
            std::nth_element(win.begin(), win.begin() + win.size() / 2, win.end());
            out[c] = win[win.size() / 2];
        }
    });
}

void motion_blur(Image& img, float angle_degrees, int strength) {
    const int n = std::max(1, strength);
    const float rad = angle_degrees * 3.14159265f / 180.0f;
    const float dx = std::cos(rad), dy = -std::sin(rad);  // screen space: angle 0 = right, 90 = up
    // Premultiplied so transparent pixels don't darken the streak.
    const Image src = img;
    const int w = img.width(), h = img.height();
    uint8_t* d = img.data();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int k = 0; k < n; ++k) {
                const int sx = std::clamp(static_cast<int>(std::lround(x + dx * k)), 0, w - 1);
                const int sy = std::clamp(static_cast<int>(std::lround(y + dy * k)), 0, h - 1);
                const uint8_t* s = src.data() + (static_cast<size_t>(sy) * w + sx) * 4;
                const float a = s[3] / 255.0f;
                acc[0] += s[0] * a; acc[1] += s[1] * a; acc[2] += s[2] * a; acc[3] += s[3];
            }
            uint8_t* p = d + (static_cast<size_t>(y) * w + x) * 4;
            const float a = acc[3] / n;
            for (int c = 0; c < 3; ++c) p[c] = a > 0 ? clamp8(acc[c] / n / (a / 255.0f)) : 0;
            p[3] = clamp8(a);
        }
}

void mosaic(Image& img, int bw, int bh) {
    bw = std::max(1, bw); bh = std::max(1, bh);
    const int w = img.width(), h = img.height();
    uint8_t* d = img.data();
    for (int by = 0; by < h; by += bh)
        for (int bx = 0; bx < w; bx += bw) {
            const int x1 = std::min(w, bx + bw), y1 = std::min(h, by + bh);
            long sum[4] = {0, 0, 0, 0};
            int n = 0;
            for (int y = by; y < y1; ++y)
                for (int x = bx; x < x1; ++x, ++n)
                    for (int c = 0; c < 4; ++c) sum[c] += d[(static_cast<size_t>(y) * w + x) * 4 + c];
            for (int y = by; y < y1; ++y)
                for (int x = bx; x < x1; ++x)
                    for (int c = 0; c < 4; ++c) d[(static_cast<size_t>(y) * w + x) * 4 + c] = static_cast<uint8_t>((sum[c] + n / 2) / n);
        }
}

void add_noise(Image& img, int percent, bool gaussian, bool monochrome, uint32_t seed) {
    uint32_t state = seed ? seed : 1;
    auto rnd = [&]() {  // xorshift32 -> [-1, 1]
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return (state & 0xFFFFFF) / static_cast<float>(0x7FFFFF) - 1.0f;
    };
    auto sample = [&]() {
        if (!gaussian) return rnd();
        float s = 0;  // approximately normal, clipped to [-1, 1]
        for (int i = 0; i < 4; ++i) s += rnd();
        return std::clamp(s * 0.5f, -1.0f, 1.0f);
    };
    const float amp = std::clamp(percent, 0, 100) * 2.55f;
    uint8_t* p = img.data();
    for (size_t i = 0; i < img.size_bytes(); i += 4) {
        if (monochrome) {
            const float n = sample() * amp;
            for (int c = 0; c < 3; ++c) p[i + c] = clamp8(p[i + c] + n);
        } else {
            for (int c = 0; c < 3; ++c) p[i + c] = clamp8(p[i + c] + sample() * amp);
        }
    }
}

void find_edges(Image& img) {
    rgb_pass(img, [&](int x, int y, auto at, float* out) {
        for (int c = 0; c < 3; ++c) {
            const float gx = -at(x - 1, y - 1)[c] - 2 * at(x - 1, y)[c] - at(x - 1, y + 1)[c] + at(x + 1, y - 1)[c] + 2 * at(x + 1, y)[c] + at(x + 1, y + 1)[c];
            const float gy = -at(x - 1, y - 1)[c] - 2 * at(x, y - 1)[c] - at(x + 1, y - 1)[c] + at(x - 1, y + 1)[c] + 2 * at(x, y + 1)[c] + at(x + 1, y + 1)[c];
            out[c] = std::sqrt(gx * gx + gy * gy) * 0.5f;
        }
    });
}

void enhance_edges(Image& img) {
    static const float k[9] = {0, -1, 0, -1, 6, -1, 0, -1, 0};
    convolve3(img, k, 2.0f);
}

void enhance_edges_more(Image& img) {
    static const float k[9] = {-1, -1, -1, -1, 10, -1, -1, -1, -1};
    convolve3(img, k, 2.0f);
}

void emboss(Image& img) {
    static const float k[9] = {-2, -1, 0, -1, 0, 1, 0, 1, 2};
    convolve3(img, k, 1.0f, 128.0f);
    // The original's emboss is gray.
    uint8_t* p = img.data();
    for (size_t i = 0; i < img.size_bytes(); i += 4) {
        const uint8_t g = static_cast<uint8_t>((p[i] * 299 + p[i + 1] * 587 + p[i + 2] * 114 + 500) / 1000);
        p[i] = p[i + 1] = p[i + 2] = g;
    }
}

void erode(Image& img) {
    rgb_pass(img, [&](int x, int y, auto at, float* out) {
        for (int c = 0; c < 3; ++c) {
            int m = 255;
            for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) m = std::min<int>(m, at(x + i, y + j)[c]);
            out[c] = static_cast<float>(m);
        }
    });
}

void dilate(Image& img) {
    rgb_pass(img, [&](int x, int y, auto at, float* out) {
        for (int c = 0; c < 3; ++c) {
            int m = 0;
            for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) m = std::max<int>(m, at(x + i, y + j)[c]);
            out[c] = static_cast<float>(m);
        }
    });
}

void drop_shadow(Image& img, int ox, int oy, float opacity, float blur, Color color) {
    const int w = img.width(), h = img.height();
    // Shadow image: the alpha mask, offset, colored.
    Image shadow(w, h, {color.r, color.g, color.b, 0});
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int sx = x - ox, sy = y - oy;
            if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
            const uint8_t a = img.data()[(static_cast<size_t>(sy) * w + sx) * 4 + 3];
            shadow.data()[(static_cast<size_t>(y) * w + x) * 4 + 3] = static_cast<uint8_t>(a * std::clamp(opacity, 0.0f, 1.0f) + 0.5f);
        }
    if (blur > 0.0f) raster::gaussian_blur(shadow, blur);
    // Original pixels over the shadow.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t* s = img.data() + (static_cast<size_t>(y) * w + x) * 4;
            raster::blend_over(shadow, x, y, {s[0], s[1], s[2], s[3]}, 1.0f);
        }
    img = std::move(shadow);
}

// --- Distortion --------------------------------------------------------


void wave(Image& img, float ha, float hw, float va, float vw) {
    const float kh = hw > 0 ? 6.2831853f / hw : 0.0f, kv = vw > 0 ? 6.2831853f / vw : 0.0f;
    remap(img, [&](float x, float y, float& sx, float& sy) {
        sx = x + (kh > 0 ? ha * std::sin(y * kh) : 0.0f);
        sy = y + (kv > 0 ? va * std::sin(x * kv) : 0.0f);
    });
}

void pinch(Image& img, int strength) {
    const float s = std::clamp(strength, -100, 100) / 100.0f;
    if (s == 0.0f) return;
    const float cx = img.width() * 0.5f, cy = img.height() * 0.5f;
    const float radius = std::min(cx, cy);
    remap(img, [&](float x, float y, float& sx, float& sy) {
        const float dx = x - cx, dy = y - cy;
        const float d = std::hypot(dx, dy);
        if (d >= radius || d == 0.0f) { sx = x; sy = y; return; }
        const float t = d / radius;
        // Pinch samples further out (pulls pixels in); punch samples closer in.
        const float f = s > 0 ? std::pow(t, 1.0f - s * 0.75f) : std::pow(t, 1.0f + (-s) * 1.5f);
        const float scale = f / t;
        sx = cx + dx * scale;
        sy = cy + dy * scale;
    });
}

void twirl(Image& img, float degrees) {
    const float cx = img.width() * 0.5f, cy = img.height() * 0.5f;
    const float radius = std::min(cx, cy);
    const float rad = degrees * 3.14159265f / 180.0f;
    remap(img, [&](float x, float y, float& sx, float& sy) {
        const float dx = x - cx, dy = y - cy;
        const float d = std::hypot(dx, dy);
        if (d >= radius) { sx = x; sy = y; return; }
        const float t = 1.0f - d / radius;
        const float a = rad * t * t;
        const float c = std::cos(a), s = std::sin(a);
        sx = cx + dx * c - dy * s;
        sy = cy + dx * s + dy * c;
    });
}

void ripple(Image& img, float amplitude, float wavelength) {
    if (wavelength <= 0.0f || amplitude == 0.0f) return;
    const float cx = img.width() * 0.5f, cy = img.height() * 0.5f;
    const float k = 6.2831853f / wavelength;
    remap(img, [&](float x, float y, float& sx, float& sy) {
        const float dx = x - cx, dy = y - cy;
        const float d = std::hypot(dx, dy);
        if (d == 0.0f) { sx = x; sy = y; return; }
        const float off = amplitude * std::sin(d * k);
        sx = x + dx / d * off;
        sy = y + dy / d * off;
    });
}

void spherize(Image& img, int strength) {
    const float s = std::clamp(strength, -100, 100) / 100.0f;
    if (s == 0.0f) return;
    const float cx = img.width() * 0.5f, cy = img.height() * 0.5f;
    const float radius = std::min(cx, cy);
    remap(img, [&](float x, float y, float& sx, float& sy) {
        const float dx = x - cx, dy = y - cy;
        const float d = std::hypot(dx, dy);
        if (d >= radius || d == 0.0f) { sx = x; sy = y; return; }
        const float t = d / radius;
        // Bulge: sample nearer the center (asin curve); dish: further out.
        const float f = s > 0 ? std::asin(t) * 2.0f / 3.14159265f : std::sin(t * 3.14159265f * 0.5f);
        const float tt = t + (f - t) * std::abs(s);
        sx = cx + dx * tt / t;
        sy = cy + dy * tt / t;
    });
}

void lens_distortion(Image& img, int strength) {
    const float s = std::clamp(strength, -100, 100) / 100.0f;
    if (s == 0.0f) return;
    const float cx = img.width() * 0.5f, cy = img.height() * 0.5f;
    const float radius = std::hypot(cx, cy);
    remap(img, [&](float x, float y, float& sx, float& sy) {
        const float dx = (x - cx) / radius, dy = (y - cy) / radius;
        const float r2 = dx * dx + dy * dy;
        const float k = 1.0f + s * 0.5f * r2;  // barrel samples further out, pincushion nearer
        sx = cx + dx * k * radius;
        sy = cy + dy * k * radius;
    });
}

void kaleidoscope(Image& img, int petals, float angle_degrees, float radius_percent) {
    petals = std::max(2, petals);
    const float cx = img.width() * 0.5f, cy = img.height() * 0.5f;
    const float wedge = 3.14159265f / petals;  // each petal is mirrored, so 2*petals wedges
    const float rot = angle_degrees * 3.14159265f / 180.0f;
    const float rmax = std::hypot(cx, cy) * std::clamp(radius_percent, 1.0f, 100.0f) / 100.0f;
    remap(img, [&](float x, float y, float& sx, float& sy) {
        const float dx = x - cx, dy = y - cy;
        const float d = std::hypot(dx, dy);
        float a = std::atan2(dy, dx) - rot;
        a = std::fmod(a, 2.0f * wedge);
        if (a < 0) a += 2.0f * wedge;
        if (a > wedge) a = 2.0f * wedge - a;  // mirror the second half of each pair
        const float dd = std::fmod(d, rmax);   // repeat outward beyond the radius
        sx = cx + dd * std::cos(a + rot);
        sy = cy + dd * std::sin(a + rot);
    });
}

void sunburst(Image& img, float fx, float fy, float brightness, int rays, float ray_brightness, Color color, uint32_t seed) {
    const int w = img.width(), h = img.height();
    const float sx = fx * w, sy = fy * h;
    const float reach = std::hypot(static_cast<float>(w), static_cast<float>(h)) * 0.5f;
    // Ray angles and strengths from a seeded generator so results repeat.
    uint32_t state = seed ? seed : 1;
    auto rnd = [&]() { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return (state & 0xFFFFFF) / 16777216.0f; };
    std::vector<float> ray_angle, ray_strength, ray_width;
    for (int i = 0; i < std::max(0, rays); ++i) { ray_angle.push_back(rnd() * 6.2831853f); ray_strength.push_back(0.3f + 0.7f * rnd()); ray_width.push_back(0.02f + 0.06f * rnd()); }
    uint8_t* p = img.data();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float dx = (x + 0.5f) - sx, dy = (y + 0.5f) - sy;
            const float d = std::hypot(dx, dy);
            float glow = brightness * std::max(0.0f, 1.0f - d / reach);
            glow *= glow;
            if (!ray_angle.empty()) {
                const float a = std::atan2(dy, dx);
                float best = 0.0f;
                for (size_t i = 0; i < ray_angle.size(); ++i) {
                    float da = std::abs(a - ray_angle[i]);
                    da = std::min(da, 6.2831853f - da);
                    best = std::max(best, ray_strength[i] * std::max(0.0f, 1.0f - da / ray_width[i]));
                }
                glow += ray_brightness * best * std::max(0.0f, 1.0f - d / (reach * 1.5f));
            }
            if (glow <= 0.0f) continue;
            uint8_t* px = p + (static_cast<size_t>(y) * w + x) * 4;
            const float k = std::min(glow, 1.0f);
            px[0] = clamp8(px[0] + (color.r - px[0]) * k);
            px[1] = clamp8(px[1] + (color.g - px[1]) * k);
            px[2] = clamp8(px[2] + (color.b - px[2]) * k);
        }
}

void halftone(Image& img, int cell, float angle_degrees, Color ink, Color paper) {
    cell = std::max(2, cell);
    const int w = img.width(), h = img.height();
    const Image src = img;
    const float rad = angle_degrees * 3.14159265f / 180.0f;
    const float cs = std::cos(rad), sn = std::sin(rad);
    uint8_t* d = img.data();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            // Rotate into the screen grid, find the cell center, rotate back.
            const float rx = x * cs + y * sn, ry = -x * sn + y * cs;
            const float gx = (std::floor(rx / cell) + 0.5f) * cell, gy = (std::floor(ry / cell) + 0.5f) * cell;
            const float ox = gx * cs - gy * sn, oy = gx * sn + gy * cs;
            const int sx = std::clamp(static_cast<int>(ox), 0, w - 1), sy = std::clamp(static_cast<int>(oy), 0, h - 1);
            const uint8_t* s = src.data() + (static_cast<size_t>(sy) * w + sx) * 4;
            const float lum = (s[0] * 299 + s[1] * 587 + s[2] * 114) / 255000.0f;
            const float dark = 1.0f - lum;
            // Dot area follows darkness; solid black reaches the cell corners.
            const float dot_r = dark > 0.0f ? std::sqrt(dark) * cell * 0.7071f + 0.5f : -1.0f;
            const float dist = std::hypot(rx - gx, ry - gy);
            const float cov = std::clamp(dot_r + 0.5f - dist, 0.0f, 1.0f);
            uint8_t* p = d + (static_cast<size_t>(y) * w + x) * 4;
            for (int c = 0; c < 3; ++c) {
                const uint8_t pc = c == 0 ? paper.r : c == 1 ? paper.g : paper.b;
                const uint8_t ic = c == 0 ? ink.r : c == 1 ? ink.g : ink.b;
                p[c] = clamp8(pc + (ic - pc) * cov);
            }
        }
}

void chrome(Image& img, int bands, float brightness) {
    bands = std::clamp(bands, 1, 20);
    uint8_t* p = img.data();
    for (size_t i = 0; i < img.size_bytes(); i += 4) {
        const float lum = (p[i] * 299 + p[i + 1] * 587 + p[i + 2] * 114) / 255000.0f;
        // Triangle wave over the luminance range: repeated highlights.
        float t = std::fmod(lum * bands, 1.0f);
        t = t < 0.5f ? t * 2.0f : 2.0f - t * 2.0f;
        const uint8_t v = clamp8(t * 255.0f * brightness);
        p[i] = p[i + 1] = p[i + 2] = v;
    }
}

// --- 3D ----------------------------------------------------------------

void buttonize(Image& img, int width, float opacity, Color color, bool transparent_edge) {
    const int w = img.width(), h = img.height();
    width = std::clamp(width, 1, std::max(1, std::min(w, h) / 2));
    opacity = std::clamp(opacity, 0.0f, 1.0f);
    uint8_t* p = img.data();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int dl = x, dr = w - 1 - x, dt = y, db = h - 1 - y;
            const int d = std::min({dl, dr, dt, db});
            if (d >= width) continue;
            // Which edge are we on? Top/left are lit, bottom/right in shadow.
            const bool lit = (dl == d && dl <= dt) || (dt == d && dt < dl);
            const float t = 1.0f - static_cast<float>(d) / width;  // 1 at the border
            uint8_t* px = p + (static_cast<size_t>(y) * w + x) * 4;
            if (transparent_edge) {
                const float k = lit ? 1.0f + 0.6f * t * opacity : 1.0f - 0.6f * t * opacity;
                for (int c = 0; c < 3; ++c) px[c] = clamp8(px[c] * k);
            } else {
                const float shade = lit ? 1.3f : 0.7f;
                const Color edge{clamp8(color.r * shade), clamp8(color.g * shade), clamp8(color.b * shade), 255};
                raster::blend_over(img, x, y, edge, opacity * (0.4f + 0.6f * t));
            }
        }
}

namespace {

// Chamfer (3-4) distance to the nearest unset pixel, in pixel units. With
// `border_is_edge` the image border also counts as an edge.
std::vector<float> distance_inside(const uint8_t* region, const Image& img, int w, int h, bool border_is_edge = true) {
    std::vector<float> d(static_cast<size_t>(w) * h, 1e9f);
    auto inside = [&](int x, int y) {
        const uint8_t v = region ? region[static_cast<size_t>(y) * w + x] : img.data()[(static_cast<size_t>(y) * w + x) * 4 + 3];
        return v >= 128;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const bool border = border_is_edge && (x == 0 || y == 0 || x == w - 1 || y == h - 1);
            if (!inside(x, y) || border) d[static_cast<size_t>(y) * w + x] = inside(x, y) ? 1.0f : 0.0f;
        }
    auto at = [&](int x, int y) -> float& { return d[static_cast<size_t>(y) * w + x]; };
    for (int y = 1; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float v = at(x, y);
            v = std::min(v, at(x, y - 1) + 1.0f);
            if (x > 0) v = std::min({v, at(x - 1, y) + 1.0f, at(x - 1, y - 1) + 1.4142f});
            if (x + 1 < w) v = std::min(v, at(x + 1, y - 1) + 1.4142f);
            at(x, y) = v;
        }
    for (int y = h - 2; y >= 0; --y)
        for (int x = w - 1; x >= 0; --x) {
            float v = at(x, y);
            v = std::min(v, at(x, y + 1) + 1.0f);
            if (x + 1 < w) v = std::min({v, at(x + 1, y) + 1.0f, at(x + 1, y + 1) + 1.4142f});
            if (x > 0) v = std::min(v, at(x - 1, y + 1) + 1.4142f);
            at(x, y) = v;
        }
    return d;
}

}  // namespace

void inner_bevel(Image& img, const uint8_t* region, int width, float angle_degrees, float depth, float ambient) {
    const int w = img.width(), h = img.height();
    width = std::max(1, width);
    const std::vector<float> dist = distance_inside(region, img, w, h);
    // Height ramp: 0 at the edge, 1 at `width` inwards (smoothstep).
    std::vector<float> height(dist.size());
    for (size_t i = 0; i < dist.size(); ++i) {
        const float t = std::clamp(dist[i] / width, 0.0f, 1.0f);
        height[i] = t * t * (3 - 2 * t);
    }
    const float rad = angle_degrees * 3.14159265f / 180.0f;
    const float lx = std::cos(rad), ly = -std::sin(rad);  // light from `angle` (0 = right, 90 = top)
    uint8_t* p = img.data();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            if (dist[i] <= 0.0f || dist[i] > width) continue;
            const float gx = height[static_cast<size_t>(y) * w + std::min(x + 1, w - 1)] - height[static_cast<size_t>(y) * w + std::max(x - 1, 0)];
            const float gy = height[static_cast<size_t>(std::min(y + 1, h - 1)) * w + x] - height[static_cast<size_t>(std::max(y - 1, 0)) * w + x];
            // The surface normal is (-gx, -gy, 1); facing the light brightens.
            const float shade = -(gx * lx + gy * ly) * depth * 4.0f;
            const float k = ambient + shade;
            uint8_t* px = p + i * 4;
            for (int c = 0; c < 3; ++c) px[c] = clamp8(px[c] * k);
        }
}

void outer_bevel(Image& img, const uint8_t* region, int width, float angle_degrees, float depth, Color color) {
    const int w = img.width(), h = img.height();
    width = std::max(1, width);
    // Distance from the region outward: invert the region and reuse the inside transform.
    std::vector<uint8_t> inverted(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t v = region ? region[static_cast<size_t>(y) * w + x] : img.data()[(static_cast<size_t>(y) * w + x) * 4 + 3];
            inverted[static_cast<size_t>(y) * w + x] = v >= 128 ? 0 : 255;
        }
    const std::vector<float> dist = distance_inside(inverted.data(), img, w, h, /*border_is_edge=*/false);
    std::vector<float> height(dist.size());
    for (size_t i = 0; i < dist.size(); ++i) {
        const float t = std::clamp(dist[i] / width, 0.0f, 1.0f);
        height[i] = 1.0f - t * t * (3 - 2 * t);  // 1 at the region edge, 0 at `width` outwards
    }
    const float rad = angle_degrees * 3.14159265f / 180.0f;
    const float lx = std::cos(rad), ly = -std::sin(rad);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            if (inverted[i] == 0 || dist[i] <= 0.0f || dist[i] > width) continue;
            const float gx = height[static_cast<size_t>(y) * w + std::min(x + 1, w - 1)] - height[static_cast<size_t>(y) * w + std::max(x - 1, 0)];
            const float gy = height[static_cast<size_t>(std::min(y + 1, h - 1)) * w + x] - height[static_cast<size_t>(std::max(y - 1, 0)) * w + x];
            const float k = 1.0f - (gx * lx + gy * ly) * depth * 4.0f;
            const Color c{clamp8(color.r * k), clamp8(color.g * k), clamp8(color.b * k), 255};
            raster::blend_over(img, x, y, c, 1.0f);
        }
}

void cutout(Image& img, const uint8_t* region, int ox, int oy, float opacity, float blur, Color color) {
    const int w = img.width(), h = img.height();
    // Shadow mask: where the offset region does NOT cover, inside the region.
    Image shadow(w, h, {color.r, color.g, color.b, 0});
    auto inside = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= w || y >= h) return false;
        const uint8_t v = region ? region[static_cast<size_t>(y) * w + x] : img.data()[(static_cast<size_t>(y) * w + x) * 4 + 3];
        return v >= 128;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            shadow.data()[(static_cast<size_t>(y) * w + x) * 4 + 3] = inside(x - ox, y - oy) ? 0 : 255;
    if (blur > 0.0f) raster::gaussian_blur(shadow, blur);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!inside(x, y)) continue;
            const uint8_t a = shadow.data()[(static_cast<size_t>(y) * w + x) * 4 + 3];
            if (a) raster::blend_over(img, x, y, {color.r, color.g, color.b, 255}, a / 255.0f * std::clamp(opacity, 0.0f, 1.0f));
        }
}

}  // namespace firn::effects
